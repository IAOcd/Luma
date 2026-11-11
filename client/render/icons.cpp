// =============================================================================
//  icons.cpp - Dibujante de íconos SVG (ver icons.h)
// =============================================================================
//  CÓMO FUNCIONA
//    1. La primera vez que se pide un ícono a un tamaño, nanosvg lee su SVG y
//       lo "rasteriza": lo convierte en píxeles, con bordes suaves
//       (antialiasing), puntas redondas y uniones redondas, igual que un
//       navegador.
//    2. Esos píxeles se copian a un "atlas": UNA sola imagen grande (1024x1024)
//       con todos los íconos usados, uno al lado del otro. Una sola textura
//       en la GPU para todos.
//    3. Dibujar el ícono es solo pegar su rectángulo del atlas con el color
//       pedido (AddImage con "tinte"): el atlas guarda los íconos en blanco,
//       y la GPU multiplica el blanco por el color. Cuesta lo mismo que una
//       letra de texto.
//
//  Rasterizar solo pasa una vez por ícono y tamaño: después es gratis. Para
//  que un ícono que cambia de tamaño en una animación no llene el atlas, los
//  tamaños grandes se redondean a escalones (los chicos van exactos, que es
//  donde un píxel de diferencia se nota).
//
//  La subida a la GPU se junta en Flush(), una vez por frame y solo si hubo
//  íconos nuevos (casi nunca: al abrir el menú por primera vez).
//
//  Si no hay "backend" (nadie dijo cómo crear texturas), se usa el dibujante
//  viejo con líneas de ImGui, para que nada quede sin ícono.
// =============================================================================
#include "render/icons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// (El código de nanosvg se compila aparte, en nanosvg_impl.cpp.)
#include <nanosvg.h>
#include <nanosvgrast.h>

namespace luma::render::icons {

namespace {

// ============================================================================
//  Parte 1: el atlas
// ============================================================================

constexpr int kAtlasSize = 1024;
constexpr int kPadding = 1;  // un píxel vacío entre íconos (que no se "contagien")

CreateFn g_create = nullptr;
UpdateFn g_update = nullptr;

// Un SVG ya leído por nanosvg (se lee una sola vez por ícono).
struct Source {
    NSVGimage* image = nullptr;
    std::vector<float> strokeWidths;  // el grosor original de cada forma
    bool ok = false;
};

// Un ícono ya rasterizado: dónde está en el atlas.
struct Cell {
    ImVec2 uv0, uv1;
    int px = 0;  // su lado en píxeles
};

struct Key {
    const char* svg;
    int px;
    int stroke;  // grosor * 16 (para poder compararlo como número entero)
    bool operator==(const Key& o) const { return svg == o.svg && px == o.px && stroke == o.stroke; }
};
struct KeyHash {
    std::size_t operator()(const Key& k) const {
        return std::hash<const void*>()(k.svg) ^ (static_cast<std::size_t>(k.px) * 2654435761u) ^
               (static_cast<std::size_t>(k.stroke) << 20);
    }
};

struct Atlas {
    std::vector<std::uint8_t> pixels;  // RGBA, kAtlasSize x kAtlasSize
    ImTextureID texture{};
    bool dirty = false;
    bool failed = false;  // no se pudo crear la textura: usar el dibujante viejo
    int x = 0, y = 0, rowHeight = 0;  // dónde va el próximo ícono (por filas)
    std::unordered_map<Key, Cell, KeyHash> cells;
    std::unordered_map<const char*, Source> sources;
    NSVGrasterizer* rasterizer = nullptr;
    std::vector<std::uint8_t> scratch;  // píxeles de un ícono recién rasterizado
};

Atlas& GetAtlas() {
    static Atlas atlas;
    return atlas;
}

// Convierte el texto de un ícono en un documento SVG completo:
//   - "<svg ...>"     -> se usa tal cual (íconos de Lucide);
//   - "M3 5H21..."    -> un solo path de trazo (los íconos viejos);
//   - "F ..."         -> el mismo path, pero RELLENO.
std::string ToDocument(const char* text, bool filled) {
    if (filled) ++text;
    while (*text == ' ') ++text;
    if (*text == '<') return text;
    std::string doc =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\">"
        "<path d=\"";
    doc += text;
    doc += filled ? "\" fill=\"#fff\" stroke=\"none\"/></svg>"
                  : "\" fill=\"none\" stroke=\"#fff\" stroke-width=\"2\" stroke-linecap=\"round\" "
                    "stroke-linejoin=\"round\"/></svg>";
    return doc;
}

Source& GetSource(Atlas& atlas, const char* svg) {
    auto found = atlas.sources.find(svg);
    if (found != atlas.sources.end()) return found->second;
    Source& source = atlas.sources[svg];
    const bool filled = svg[0] == 'F';
    std::string doc = ToDocument(svg, filled);
    // nanosvg escribe sobre el texto mientras lo lee: le damos una copia.
    std::vector<char> copy(doc.begin(), doc.end());
    copy.push_back('\0');
    source.image = nsvgParse(copy.data(), "px", 96.0f);
    if (source.image == nullptr || source.image->width <= 0.0f) return source;
    for (NSVGshape* shape = source.image->shapes; shape != nullptr; shape = shape->next) {
        // Todo en blanco (el color lo pone el "tinte" al dibujar). Lucide
        // usa "currentColor", que nanosvg no conoce: lo cambiamos aquí.
        if (shape->stroke.type != NSVG_PAINT_NONE) {
            shape->stroke.type = NSVG_PAINT_COLOR;
            shape->stroke.color = 0xFFFFFFFFu;
        }
        if (filled) {
            // "F" sobre un ícono de Lucide: además del trazo, rellenar la forma
            // (el botón de play lleno, por ejemplo).
            shape->fill.type = NSVG_PAINT_COLOR;
            shape->fill.color = 0xFFFFFFFFu;
        } else if (shape->fill.type != NSVG_PAINT_NONE) {
            shape->fill.type = NSVG_PAINT_COLOR;
            shape->fill.color = 0xFFFFFFFFu;
        }
        source.strokeWidths.push_back(shape->strokeWidth);
    }
    source.ok = true;
    return source;
}

// Los tamaños grandes van en escalones (para que una animación de tamaño no
// cree cien versiones del mismo ícono). Los chicos, exactos.
int BucketSize(float size) {
    int px = static_cast<int>(std::lround(size));
    if (px <= 40) return std::max(px, 4);
    if (px <= 80) return (px + 3) / 4 * 4;
    return std::min((px + 7) / 8 * 8, 192);
}

void ClearAtlas(Atlas& atlas) {
    std::fill(atlas.pixels.begin(), atlas.pixels.end(), static_cast<std::uint8_t>(0));
    atlas.cells.clear();
    atlas.x = atlas.y = atlas.rowHeight = 0;
    atlas.dirty = true;
}

// Rasteriza un ícono a "px" píxeles de lado y lo pone en el atlas.
const Cell* Rasterize(Atlas& atlas, const Key& key, float strokeUnits) {
    Source& source = GetSource(atlas, key.svg);
    if (!source.ok) return nullptr;
    if (atlas.rasterizer == nullptr) atlas.rasterizer = nsvgCreateRasterizer();
    if (atlas.rasterizer == nullptr) return nullptr;

    // ¿Cabe en la fila actual? Si no, fila nueva; si no hay más filas, el
    // atlas se llenó: se vacía y se empieza de nuevo (rarísimo).
    const int px = key.px;
    if (atlas.x + px + kPadding > kAtlasSize) {
        atlas.x = 0;
        atlas.y += atlas.rowHeight + kPadding;
        atlas.rowHeight = 0;
    }
    if (atlas.y + px + kPadding > kAtlasSize) {
        ClearAtlas(atlas);
    }

    // El grosor pedido, aplicado a cada forma (proporcional al original: si
    // el ícono tiene partes más finas, siguen más finas).
    int i = 0;
    for (NSVGshape* shape = source.image->shapes; shape != nullptr; shape = shape->next, ++i) {
        shape->strokeWidth = source.strokeWidths[i] * (strokeUnits / 2.0f);
    }
    atlas.scratch.assign(static_cast<std::size_t>(px) * px * 4, 0);
    const float scale = static_cast<float>(px) / source.image->width;
    nsvgRasterize(atlas.rasterizer, source.image, 0.0f, 0.0f, scale, atlas.scratch.data(), px, px, px * 4);

    // Copiar al atlas: blanco puro, y la forma en el canal alfa.
    for (int row = 0; row < px; ++row) {
        std::uint8_t* dst = atlas.pixels.data() + (static_cast<std::size_t>(atlas.y + row) * kAtlasSize + atlas.x) * 4;
        const std::uint8_t* src = atlas.scratch.data() + static_cast<std::size_t>(row) * px * 4;
        for (int col = 0; col < px; ++col) {
            dst[col * 4 + 0] = 255;
            dst[col * 4 + 1] = 255;
            dst[col * 4 + 2] = 255;
            dst[col * 4 + 3] = src[col * 4 + 3];
        }
    }
    const float inv = 1.0f / kAtlasSize;
    Cell cell;
    cell.px = px;
    cell.uv0 = ImVec2(atlas.x * inv, atlas.y * inv);
    cell.uv1 = ImVec2((atlas.x + px) * inv, (atlas.y + px) * inv);
    atlas.x += px + kPadding;
    atlas.rowHeight = std::max(atlas.rowHeight, px);
    atlas.dirty = true;
    return &(atlas.cells[key] = cell);
}

// ============================================================================
//  Parte 2: el dibujante viejo (si no hay texturas)
// ============================================================================
//  Lee el path y lo dibuja con líneas y curvas de ImGui. Se ve peor (sin
//  antialiasing de verdad ni puntas redondas), pero nunca falla.

struct Segment {
    enum Type { Move, Line, Cubic, Close } type;
    ImVec2 p[3];
};
using Parsed = std::vector<Segment>;

bool ReadNumber(const char*& s, float& out) {
    while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t') ++s;
    if (!(*s == '-' || *s == '+' || *s == '.' || std::isdigit(static_cast<unsigned char>(*s)))) return false;
    char* end = nullptr;
    out = std::strtof(s, &end);
    if (end == s) return false;
    s = end;
    return true;
}

Parsed ParsePath(const char* s) {
    Parsed result;
    ImVec2 current(0, 0), start(0, 0), lastControl(0, 0);
    char command = 0;
    while (*s != '\0') {
        while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t') ++s;
        if (*s == '\0') break;
        if (std::isalpha(static_cast<unsigned char>(*s))) command = *s++;
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const ImVec2 base = relative ? current : ImVec2(0, 0);
        float a[6];
        switch (std::toupper(static_cast<unsigned char>(command))) {
            case 'M':
                if (!ReadNumber(s, a[0]) || !ReadNumber(s, a[1])) return result;
                current = ImVec2(base.x + a[0], base.y + a[1]);
                start = current;
                result.push_back({Segment::Move, {current}});
                command = relative ? 'l' : 'L';
                break;
            case 'L':
                if (!ReadNumber(s, a[0]) || !ReadNumber(s, a[1])) return result;
                current = ImVec2(base.x + a[0], base.y + a[1]);
                result.push_back({Segment::Line, {current}});
                break;
            case 'H':
                if (!ReadNumber(s, a[0])) return result;
                current.x = (relative ? current.x : 0.0f) + a[0];
                result.push_back({Segment::Line, {current}});
                break;
            case 'V':
                if (!ReadNumber(s, a[0])) return result;
                current.y = (relative ? current.y : 0.0f) + a[0];
                result.push_back({Segment::Line, {current}});
                break;
            case 'C': {
                for (int i = 0; i < 6; ++i)
                    if (!ReadNumber(s, a[i])) return result;
                const ImVec2 c1(base.x + a[0], base.y + a[1]), c2(base.x + a[2], base.y + a[3]),
                    end(base.x + a[4], base.y + a[5]);
                result.push_back({Segment::Cubic, {c1, c2, end}});
                lastControl = c2;
                current = end;
                continue;
            }
            case 'S': {
                for (int i = 0; i < 4; ++i)
                    if (!ReadNumber(s, a[i])) return result;
                const ImVec2 c1(2.0f * current.x - lastControl.x, 2.0f * current.y - lastControl.y);
                const ImVec2 c2(base.x + a[0], base.y + a[1]), end(base.x + a[2], base.y + a[3]);
                result.push_back({Segment::Cubic, {c1, c2, end}});
                lastControl = c2;
                current = end;
                continue;
            }
            case 'Q': {
                for (int i = 0; i < 4; ++i)
                    if (!ReadNumber(s, a[i])) return result;
                const ImVec2 q(base.x + a[0], base.y + a[1]), end(base.x + a[2], base.y + a[3]);
                const ImVec2 c1(current.x + (q.x - current.x) * 2.0f / 3.0f, current.y + (q.y - current.y) * 2.0f / 3.0f);
                const ImVec2 c2(end.x + (q.x - end.x) * 2.0f / 3.0f, end.y + (q.y - end.y) * 2.0f / 3.0f);
                result.push_back({Segment::Cubic, {c1, c2, end}});
                current = end;
                break;
            }
            case 'Z':
                result.push_back({Segment::Close, {start}});
                current = start;
                break;
            default:
                return result;
        }
        lastControl = current;
    }
    return result;
}

void DrawLegacy(ImDrawList* draw, const char* svg, ImVec2 center, float size, ImU32 color, float thickness) {
    const bool filled = svg[0] == 'F';
    const char* path = filled ? svg + 1 : svg;
    while (*path == ' ') ++path;
    if (*path == '<') return;  // un SVG completo: este dibujante no lo entiende
    static std::vector<std::pair<const char*, Parsed>> cache;
    const Parsed* segments = nullptr;
    for (const auto& [key, parsed] : cache) {
        if (key == svg) segments = &parsed;
    }
    if (segments == nullptr) {
        cache.emplace_back(svg, ParsePath(path));
        segments = &cache.back().second;
    }
    const float scale = size / 24.0f;
    const ImVec2 origin(center.x - size * 0.5f, center.y - size * 0.5f);
    auto map = [&](ImVec2 p) { return ImVec2(origin.x + p.x * scale, origin.y + p.y * scale); };
    bool open = false;
    auto flush = [&](bool closed) {
        if (!open) return;
        if (filled) draw->PathFillConcave(color);
        else draw->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
        open = false;
    };
    for (const Segment& seg : *segments) {
        switch (seg.type) {
            case Segment::Move:
                flush(false);
                draw->PathLineTo(map(seg.p[0]));
                open = true;
                break;
            case Segment::Line: draw->PathLineTo(map(seg.p[0])); break;
            case Segment::Cubic: draw->PathBezierCubicCurveTo(map(seg.p[0]), map(seg.p[1]), map(seg.p[2])); break;
            case Segment::Close: flush(true); break;
        }
    }
    flush(false);
}

}  // namespace

void SetBackend(CreateFn create, UpdateFn update) {
    g_create = create;
    g_update = update;
}

void Draw(ImDrawList* draw, const char* svg, ImVec2 center, float size, ImU32 color, float thickness) {
    if (svg == nullptr || draw == nullptr || size < 1.0f || (color & IM_COL32_A_MASK) == 0) return;
    // El grosor pedido (en píxeles) pasado a la cuadrícula de 24x24 del ícono.
    // 0 = el de Lucide (2 de 24).
    const float strokeUnits = thickness > 0.0f ? std::clamp(thickness * 24.0f / size, 0.75f, 3.5f) : 2.0f;

    Atlas& atlas = GetAtlas();
    if (g_create == nullptr || atlas.failed) {
        DrawLegacy(draw, svg, center, size, color, thickness > 0.0f ? thickness : std::max(1.2f, size * 0.083f));
        return;
    }
    if (atlas.texture == ImTextureID{}) {
        // La primera vez: crear la textura (vacía) del atlas.
        atlas.pixels.assign(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4, 0);
        atlas.texture = g_create(atlas.pixels.data(), kAtlasSize, kAtlasSize);
        if (atlas.texture == ImTextureID{}) {
            atlas.failed = true;
            DrawLegacy(draw, svg, center, size, color, std::max(1.2f, size * 0.083f));
            return;
        }
    }

    const Key key{svg, BucketSize(size), static_cast<int>(std::lround(strokeUnits * 16.0f))};
    const Cell* cell = nullptr;
    const auto found = atlas.cells.find(key);
    if (found != atlas.cells.end()) {
        cell = &found->second;
    } else {
        cell = Rasterize(atlas, key, strokeUnits);
        if (cell == nullptr) {
            DrawLegacy(draw, svg, center, size, color, thickness > 0.0f ? thickness : std::max(1.2f, size * 0.083f));
            return;
        }
    }
    // Pegado a píxeles enteros cuando el tamaño es exacto: así las líneas
    // finas caen justas en la grilla de la pantalla y se ven nítidas.
    ImVec2 min(center.x - size * 0.5f, center.y - size * 0.5f);
    if (static_cast<float>(cell->px) == std::round(size)) {
        min = ImVec2(std::round(min.x), std::round(min.y));
        size = static_cast<float>(cell->px);
    }
    draw->AddImage(atlas.texture, min, ImVec2(min.x + size, min.y + size), cell->uv0, cell->uv1, color);
}

void Flush() {
    Atlas& atlas = GetAtlas();
    if (!atlas.dirty || atlas.texture == ImTextureID{} || g_update == nullptr) return;
    atlas.dirty = false;
    if (!g_update(atlas.texture, atlas.pixels.data(), kAtlasSize, kAtlasSize)) {
        // No se pudo subir: de ahora en más, el dibujante viejo.
        atlas.failed = true;
    }
}

void Shutdown() {
    Atlas& atlas = GetAtlas();
    for (auto& [svg, source] : atlas.sources) {
        if (source.image != nullptr) nsvgDelete(source.image);
    }
    atlas.sources.clear();
    atlas.cells.clear();
    if (atlas.rasterizer != nullptr) nsvgDeleteRasterizer(atlas.rasterizer);
    atlas.rasterizer = nullptr;
    atlas.texture = ImTextureID{};  // la textura la libera quien la creó (gpu::Shutdown)
    atlas.pixels.clear();
    atlas.pixels.shrink_to_fit();
    atlas.dirty = false;
    atlas.failed = false;
    atlas.x = atlas.y = atlas.rowHeight = 0;
}

}  // namespace luma::render::icons
