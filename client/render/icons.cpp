// =============================================================================
//  icons.cpp - Lector y dibujante de paths SVG
// =============================================================================
#include "render/icons.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>
#include <vector>

namespace luma::render::icons {

namespace {

// Un tramo del dibujo, ya en coordenadas absolutas de la cuadrícula 24x24.
struct Segment {
    enum Type { Move, Line, Cubic, Close } type;
    ImVec2 p[3];  // Line: p[0]; Cubic: control1, control2, fin; Move: p[0]
};

using Parsed = std::vector<Segment>;

// Lee el siguiente número del texto (salta espacios y comas).
bool ReadNumber(const char*& s, float& out) {
    while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t') {
        ++s;
    }
    if (!(*s == '-' || *s == '+' || *s == '.' || std::isdigit(static_cast<unsigned char>(*s)))) {
        return false;
    }
    char* end = nullptr;
    out = std::strtof(s, &end);
    if (end == s) {
        return false;
    }
    s = end;
    return true;
}

Parsed Parse(const char* s) {
    Parsed result;
    ImVec2 current(0, 0);
    ImVec2 start(0, 0);
    ImVec2 lastControl(0, 0);  // para S (curva suave): refleja el último control
    char command = 0;
    while (*s != '\0') {
        while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t') {
            ++s;
        }
        if (*s == '\0') {
            break;
        }
        if (std::isalpha(static_cast<unsigned char>(*s))) {
            command = *s++;
        }
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const ImVec2 base = relative ? current : ImVec2(0, 0);
        float a[6];
        switch (std::toupper(static_cast<unsigned char>(command))) {
            case 'M':
                if (!ReadNumber(s, a[0]) || !ReadNumber(s, a[1])) return result;
                current = ImVec2(base.x + a[0], base.y + a[1]);
                start = current;
                result.push_back({Segment::Move, {current}});
                // Números extra después de M = líneas (regla del formato SVG).
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
                for (int i = 0; i < 6; ++i) {
                    if (!ReadNumber(s, a[i])) return result;
                }
                const ImVec2 c1(base.x + a[0], base.y + a[1]);
                const ImVec2 c2(base.x + a[2], base.y + a[3]);
                const ImVec2 end(base.x + a[4], base.y + a[5]);
                result.push_back({Segment::Cubic, {c1, c2, end}});
                lastControl = c2;
                current = end;
                continue;  // no resetear lastControl
            }
            case 'S': {
                for (int i = 0; i < 4; ++i) {
                    if (!ReadNumber(s, a[i])) return result;
                }
                const ImVec2 c1(2.0f * current.x - lastControl.x, 2.0f * current.y - lastControl.y);
                const ImVec2 c2(base.x + a[0], base.y + a[1]);
                const ImVec2 end(base.x + a[2], base.y + a[3]);
                result.push_back({Segment::Cubic, {c1, c2, end}});
                lastControl = c2;
                current = end;
                continue;
            }
            case 'Q': {
                for (int i = 0; i < 4; ++i) {
                    if (!ReadNumber(s, a[i])) return result;
                }
                // Una curva cuadrática se puede escribir como cúbica.
                const ImVec2 q(base.x + a[0], base.y + a[1]);
                const ImVec2 end(base.x + a[2], base.y + a[3]);
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
                return result;  // comando no soportado: dibujamos lo que haya
        }
        lastControl = current;
    }
    return result;
}

// Los paths ya leídos, para no volver a leerlos cada frame. Se buscan por
// la dirección del texto (los íconos son constantes que no cambian de lugar).
const Parsed& Get(const char* svgPath) {
    static std::vector<std::pair<const char*, Parsed>> cache;
    for (const auto& [key, parsed] : cache) {
        if (key == svgPath) {
            return parsed;
        }
    }
    cache.emplace_back(svgPath, Parse(svgPath[0] == 'F' ? svgPath + 1 : svgPath));
    return cache.back().second;
}

}  // namespace

void Draw(ImDrawList* draw, const char* svgPath, ImVec2 center, float size, ImU32 color, float thickness) {
    if (svgPath == nullptr) {
        return;
    }
    // Un path que empieza con "F " es una FORMA RELLENA (como el logo de
    // Luma) en vez de un trazo de líneas.
    const bool filled = svgPath[0] == 'F';
    const Parsed& segments = Get(svgPath);
    const float scale = size / 24.0f;
    const ImVec2 origin(center.x - size * 0.5f, center.y - size * 0.5f);
    auto map = [&](ImVec2 p) { return ImVec2(origin.x + p.x * scale, origin.y + p.y * scale); };
    if (thickness <= 0.0f) {
        thickness = std::max(1.2f, size * 0.075f);
    }

    // Recorremos los tramos y vamos "trazando" con la API de paths de ImGui.
    // Cada M empieza un trazo nuevo; Z lo cierra.
    bool open = false;
    auto flush = [&](bool closed) {
        if (open && filled) {
            // PathFillConcave rellena formas "con entrantes" (no convexas).
            draw->PathFillConcave(color);
            open = false;
        } else if (open) {
            draw->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
            open = false;
        }
    };
    for (const Segment& seg : segments) {
        switch (seg.type) {
            case Segment::Move:
                flush(false);
                draw->PathLineTo(map(seg.p[0]));
                open = true;
                break;
            case Segment::Line:
                draw->PathLineTo(map(seg.p[0]));
                break;
            case Segment::Cubic:
                draw->PathBezierCubicCurveTo(map(seg.p[0]), map(seg.p[1]), map(seg.p[2]));
                break;
            case Segment::Close:
                flush(true);
                break;
        }
    }
    flush(false);
}

}  // namespace luma::render::icons
