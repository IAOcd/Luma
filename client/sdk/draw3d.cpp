// =============================================================================
//  draw3d.cpp - Dibujo dentro del mundo (ver draw3d.h)
// =============================================================================
#include "sdk/draw3d.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "core/hooks.h"
#include "core/logger.h"
#include "sdk/entity.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"
#include "sdk/scanner.h"

namespace luma::sdk::draw3d {

namespace {

// --- Firmas (1.26.6x) --------------------------------------------------------------
constexpr const char* kTessBegin =
    "56 57 55 53 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 80 B9 ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? "
    "48 89 CE 80 B9";
constexpr const char* kTessVertex =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC ?? 44 0F 29 4C 24 ?? 44 0F 29 44 24 ?? 0F 29 7C 24 ?? 0F 29 74 24 ?? 8B 81";
constexpr const char* kRenderMesh =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC ?? ?? ?? ?? 48 8D AC 24 ?? ?? ?? ?? 48 C7 85 ?? ?? ?? ?? ?? ?? ?? ?? "
    "80 BA ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 4C 89 CF";
// Un lugar del juego que usa el grupo de materiales "common" (lea rdx, [grupo]).
constexpr const char* kMaterialGroupRef =
    "48 8D 15 ?? ?? ?? ?? 4C 8D 45 ?? E8 ?? ?? ?? ?? 90 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 48 8D 0D";
constexpr const char* kRenderOutline =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC ?? ?? ?? ?? 48 8D AC 24 ?? ?? ?? ?? 44 0F 29 85 ?? ?? ?? ?? "
    "0F 29 BD ?? ?? ?? ?? 0F 29 B5 ?? ?? ?? ?? 48 C7 85 ?? ?? ?? ?? ?? ?? ?? ?? 4C 89 CB 4D 89 C7";

// --- Estructuras del juego (offsets en 1.26.63) -------------------------------------
constexpr std::uintptr_t kScreenContextColor = 0x30;  // -> float[4]: color con que se tiñe lo que se dibuja
constexpr std::uintptr_t kScreenContextTess = 0xB0;   // -> Tessellator
constexpr std::uintptr_t kLevelRendererPlayer = 0x480;
constexpr std::uintptr_t kCameraOrigin = 0x658;       // Vec3: desde dónde mira la cámara
constexpr std::uintptr_t kTessFormatFixed = 0x0;      // bool
constexpr std::uintptr_t kTessEnabledFields = 0x158;  // bool[15]: [1] = "tiene color"
constexpr std::uintptr_t kTessNextColor = 0x1B0;      // std::optional<uint32>: {valor, ¿tiene valor?}
constexpr std::uintptr_t kTessNoColor = 0x254;        // bool
constexpr int kPrimitiveLineList = 4;
constexpr int kPrimitiveQuad = 1;

// AABBShapeComponent (las cajas de las entidades) en el sistema de entidades.
constexpr std::uint32_t kAabbHash = 0xBAC1B3CF;
constexpr std::uintptr_t kAabbComponentSize = 0x20;  // Vec3 min, Vec3 max, Vec2 tamaño
// ActorTypeComponent: el tipo de entidad (un número; 319 = jugador).
constexpr std::uint32_t kActorTypeHash = 0x4F6BA419;
constexpr std::uint32_t kTypePlayer = 319;
constexpr std::uint32_t kTypeItem = 64;
constexpr std::uint32_t kTypeXpOrb = 69;

using TessBeginFn = void(__fastcall*)(void* tess, int unused, int primitive, int vertexCount, bool buildFaceData);
using TessVertexFn = void(__fastcall*)(void* tess, float x, float y, float z);
using RenderMeshFn = void(__fastcall*)(void* screenContext, void* tess, void* material, char* pad);
using RenderOutlineFn = void(__fastcall*)(void* lrp, void* screenContext, void* block, void* region, const int* pos);
using CreateMaterialFn = void(__fastcall*)(void* group, void* outSharedPtr, const void* hashedName);

TessBeginFn g_begin = nullptr;
TessVertexFn g_vertex = nullptr;
RenderMeshFn g_renderMesh = nullptr;
RenderOutlineFn g_renderOutline = nullptr;
std::uintptr_t g_materialGroup = 0;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_outlineHooked{false};

// Los materiales que pedimos al juego. Guardamos el "shared_ptr" crudo (dos
// punteros) y nunca lo liberamos: es un objeto del juego que vive mientras
// el juego viva.
struct RawSharedPtr {
    void* object = nullptr;
    void* control = nullptr;
};
RawSharedPtr g_boxMaterial;
RawSharedPtr g_overlayMaterial;

std::mutex g_settingsMutex;
OutlineSettings g_outline;
HitboxSettings g_hitboxes;

// --- HashedString (un texto con su "huella" precalculada) -----------------------------
std::uint64_t Fnv1a64(const char* text) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char* c = text; *c != 0; ++c) {
        h = (h ^ static_cast<unsigned char>(*c)) * 0x100000001B3ull;
    }
    return h;
}

struct HashedString {
    std::uint64_t hash;
    std::string text;
    const void* lastMatch = nullptr;
};

// Llama a la función del juego que crea un material (vtable[1] del grupo).
// Aparte, porque __try no se puede usar junto a objetos con destructor.
bool CallCreateMaterial(std::uintptr_t fn, RawSharedPtr* out, const HashedString* name) {
    __try {
        reinterpret_cast<CreateMaterialFn>(fn)(reinterpret_cast<void*>(g_materialGroup), out, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = RawSharedPtr{};
        return false;
    }
}

RawSharedPtr CreateMaterial(const char* name) {
    RawSharedPtr out;
    if (g_materialGroup == 0) {
        return out;
    }
    const HashedString hashed{Fnv1a64(name), name, nullptr};
    std::uintptr_t vtable = 0;
    std::uintptr_t fn = 0;
    if (!SafeRead(g_materialGroup, vtable) || !SafeRead(vtable + 8, fn) || fn == 0) {
        return out;
    }
    CallCreateMaterial(fn, &out, &hashed);
    return out;
}

// Pide los materiales al juego la primera vez (en el hilo de render, como él).
bool EnsureMaterials() {
    static bool tried = false;
    if (!tried) {
        tried = true;
        g_boxMaterial = CreateMaterial("selection_box");
        g_overlayMaterial = CreateMaterial("selection_overlay");
        log::Info("SDK: materiales del mundo: selection_box {}, selection_overlay {}.",
                  g_boxMaterial.object != nullptr ? "sí" : "no", g_overlayMaterial.object != nullptr ? "sí" : "no");
    }
    return g_boxMaterial.object != nullptr;
}

// --- Armar y dibujar ------------------------------------------------------------------
struct Frame {
    std::uintptr_t sc = 0;
    std::uintptr_t tess = 0;
    float origin[3]{};
};

void SetColor(const Frame& f, const Color& c) {
    std::uint8_t noColor = 0;
    SafeRead(f.tess + kTessNoColor, noColor);
    if (noColor != 0) {
        return;
    }
    const std::uint32_t packed = (static_cast<std::uint32_t>(c.a * 255.0f) << 24) |
                                 (static_cast<std::uint32_t>(c.b * 255.0f) << 16) |
                                 (static_cast<std::uint32_t>(c.g * 255.0f) << 8) | static_cast<std::uint32_t>(c.r * 255.0f);
    *reinterpret_cast<std::uint32_t*>(f.tess + kTessNextColor) = packed;
    *reinterpret_cast<std::uint8_t*>(f.tess + kTessNextColor + 4) = 1;  // std::optional: "tiene valor"
    std::uint8_t fixed = 0;
    SafeRead(f.tess + kTessFormatFixed, fixed);
    if (fixed == 0) {
        *reinterpret_cast<std::uint8_t*>(f.tess + kTessEnabledFields + 1) = 1;
    }
}

// DIAGNÓSTICO (una vez): qué bytes del Tessellator cambian en cada paso. Sirve
// para comprobar en esta versión dónde guarda el color y los vértices.
bool g_diagDone = false;
void LogDiff(const char* step, const std::uint8_t* before, const std::uint8_t* after, std::size_t size) {
    char line[700];
    int n = std::snprintf(line, sizeof(line), "SDK: Tessellator tras %s:", step);
    int shown = 0;
    for (std::size_t i = 0; i < size && shown < 40 && n < 640; ++i) {
        if (before[i] != after[i]) {
            n += std::snprintf(line + n, sizeof(line) - n, " +%zX:%02X>%02X", i, before[i], after[i]);
            ++shown;
        }
    }
    log::Info("{}", line);
}

void Line(const Frame& f, const Color& c, float x1, float y1, float z1, float x2, float y2, float z2) {
    std::uint8_t s0[0x300], s1[0x300], s2[0x300], s3[0x300];
    const bool diag = !g_diagDone && SafeCopy(f.tess, s0, sizeof(s0));
    g_begin(reinterpret_cast<void*>(f.tess), 0, kPrimitiveLineList, 1, false);
    if (diag) SafeCopy(f.tess, s1, sizeof(s1));
    SetColor(f, c);
    if (diag) SafeCopy(f.tess, s2, sizeof(s2));
    g_vertex(reinterpret_cast<void*>(f.tess), x1 - f.origin[0], y1 - f.origin[1], z1 - f.origin[2]);
    g_vertex(reinterpret_cast<void*>(f.tess), x2 - f.origin[0], y2 - f.origin[1], z2 - f.origin[2]);
    if (diag && SafeCopy(f.tess, s3, sizeof(s3))) {
        g_diagDone = true;
        LogDiff("begin", s0, s1, sizeof(s0));
        LogDiff("color", s1, s2, sizeof(s0));
        LogDiff("2 vértices", s2, s3, sizeof(s0));
    }
}

// Las 12 aristas de una caja.
void Box(const Frame& f, const Color& c, const float* lo, const float* hi) {
    const float x0 = lo[0], y0 = lo[1], z0 = lo[2], x1 = hi[0], y1 = hi[1], z1 = hi[2];
    // abajo
    Line(f, c, x0, y0, z0, x1, y0, z0);
    Line(f, c, x1, y0, z0, x1, y0, z1);
    Line(f, c, x1, y0, z1, x0, y0, z1);
    Line(f, c, x0, y0, z1, x0, y0, z0);
    // arriba
    Line(f, c, x0, y1, z0, x1, y1, z0);
    Line(f, c, x1, y1, z0, x1, y1, z1);
    Line(f, c, x1, y1, z1, x0, y1, z1);
    Line(f, c, x0, y1, z1, x0, y1, z0);
    // columnas
    Line(f, c, x0, y0, z0, x0, y1, z0);
    Line(f, c, x1, y0, z0, x1, y1, z0);
    Line(f, c, x1, y0, z1, x1, y1, z1);
    Line(f, c, x0, y0, z1, x0, y1, z1);
}

// Las 6 caras de una caja, rellenas (para el relleno del contorno).
void FilledBox(const Frame& f, const Color& c, const float* lo, const float* hi) {
    const float x0 = lo[0] - f.origin[0], y0 = lo[1] - f.origin[1], z0 = lo[2] - f.origin[2];
    const float x1 = hi[0] - f.origin[0], y1 = hi[1] - f.origin[1], z1 = hi[2] - f.origin[2];
    const float faces[6][4][3] = {
        {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}, {{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}},
        {{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}, {{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}},
        {{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}, {{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}},
    };
    for (const auto& face : faces) {
        g_begin(reinterpret_cast<void*>(f.tess), 0, kPrimitiveQuad, 8, false);
        SetColor(f, c);
        // Las dos caras de cada cuadrado (se ve desde adentro y desde afuera).
        for (int i = 0; i < 4; ++i) g_vertex(reinterpret_cast<void*>(f.tess), face[i][0], face[i][1], face[i][2]);
        for (int i = 3; i >= 0; --i) g_vertex(reinterpret_cast<void*>(f.tess), face[i][0], face[i][1], face[i][2]);
    }
}

bool MakeFrame(std::uintptr_t levelRenderer, std::uintptr_t screenContext, Frame& f) {
    std::uintptr_t lrp = 0;
    f.sc = screenContext;
    return SafeRead(screenContext + kScreenContextTess, f.tess) && LooksLikePointer(f.tess) &&
           SafeRead(levelRenderer + kLevelRendererPlayer, lrp) && LooksLikePointer(lrp) &&
           SafeRead(lrp + kCameraOrigin, f.origin) && std::isfinite(f.origin[0]);
}

void Render(const Frame& f, const RawSharedPtr& material) {
    // Sin teñido extra de la pantalla (blanco = los colores tal cual).
    std::uintptr_t colorPtr = 0;
    if (SafeRead(f.sc + kScreenContextColor, colorPtr) && LooksLikePointer(colorPtr)) {
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::memcpy(reinterpret_cast<void*>(colorPtr), white, sizeof(white));
    }
    char pad[0x58]{};
    g_renderMesh(reinterpret_cast<void*>(f.sc), reinterpret_cast<void*>(f.tess), material.object, pad);
}

// --- Hitboxes: recorrer todas las cajas de entidades --------------------------------
void DrawHitboxes(const Frame& f, const HitboxSettings& s) {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t registry = 0;
    if (player == 0 || !SafeRead(player + offsets::kActorEnttRegistry, registry) || !LooksLikePointer(registry)) {
        return;
    }
    // Las piscinas de cajas y de tipos de entidad.
    const std::uintptr_t boxes = entity::Storage(registry, kAabbHash);
    const std::uintptr_t types = entity::Storage(registry, kActorTypeHash);
    std::uintptr_t entities[2]{};  // el vector de entidades de la piscina de cajas
    std::uintptr_t pages = 0;
    if (boxes == 0 || !SafeRead(boxes + 0x20, entities) || !SafeRead(boxes + offsets::kStoragePayload, pages) ||
        entities[1] < entities[0]) {
        return;
    }
    std::uintptr_t ownAabb = 0;  // tu propia caja (para saltearla)
    SafeRead(player + offsets::kActorAabb, ownAabb);
    const std::size_t count = std::min<std::size_t>((entities[1] - entities[0]) / 4, 4096);
    const float maxDist2 = s.maxDistance * s.maxDistance;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t entity = 0;
        if (!SafeRead(entities[0] + i * 4, entity) || (entity & offsets::kEntityMask) == offsets::kEntityMask) {
            continue;  // lugar vacío
        }
        std::uintptr_t page = 0;
        if (!SafeRead(pages + (i / offsets::kPayloadPage) * 8, page) || !LooksLikePointer(page)) {
            continue;
        }
        const std::uintptr_t comp = page + (i % offsets::kPayloadPage) * kAabbComponentSize;
        if (comp == ownAabb && !s.showSelf) {
            continue;
        }
        float box[8]{};
        if (!SafeRead(comp, box) || !std::isfinite(box[0]) || !std::isfinite(box[4])) {
            continue;
        }
        const float width = box[6];
        const float height = box[7];
        if (width <= 0.0f || height <= 0.0f || width > 8.0f || height > 8.0f) {
            continue;
        }
        const float cx = (box[0] + box[3]) * 0.5f - f.origin[0];
        const float cy = (box[1] + box[4]) * 0.5f - f.origin[1];
        const float cz = (box[2] + box[5]) * 0.5f - f.origin[2];
        if (cx * cx + cy * cy + cz * cz > maxDist2) {
            continue;
        }
        // ¿Qué es? (jugador, ítem/orbe o "mob": todo lo demás)
        std::uint32_t type = 0;
        const std::uintptr_t typeComp = entity::ComponentOf(types, entity, 4);
        if (typeComp != 0) {
            SafeRead(typeComp, type);
        }
        const bool isPlayer = (type & 0xFFFF) == kTypePlayer || comp == ownAabb;
        const bool isItem = (type & 0xFFFF) == kTypeItem || (type & 0xFFFF) == kTypeXpOrb ||
                            (typeComp == 0 && width < 0.4f && height < 0.4f);
        if (isPlayer) {
            if (s.players) Box(f, s.playerColor, box, box + 3);
        } else if (isItem) {
            if (s.items) Box(f, s.itemColor, box, box + 3);
        } else if (s.mobs) {
            Box(f, s.mobColor, box, box + 3);
        }
    }
}

// --- Contorno de bloque: reemplazar el del juego ------------------------------------
OutlineSettings CopyOutline() {
    std::lock_guard lock(g_settingsMutex);
    return g_outline;
}

HitboxSettings CopyHitboxes() {
    std::lock_guard lock(g_settingsMutex);
    return g_hitboxes;
}

void DrawOutline(void* lrp, void* screenContext, void* block, void* region, const int* pos, const OutlineSettings& s) {
    __try {
        Frame f;
        f.sc = reinterpret_cast<std::uintptr_t>(screenContext);
        if (!SafeRead(f.sc + kScreenContextTess, f.tess) || !LooksLikePointer(f.tess) ||
            !SafeRead(reinterpret_cast<std::uintptr_t>(lrp) + kCameraOrigin, f.origin)) {
            g_renderOutline(lrp, screenContext, block, region, pos);
            return;
        }
        // Un poco más grande que el bloque, para que no "pelee" con sus caras.
        const float lo[3] = {pos[0] - 0.002f, pos[1] - 0.002f, pos[2] - 0.002f};
        const float hi[3] = {pos[0] + 1.002f, pos[1] + 1.002f, pos[2] + 1.002f};
        if (s.fill && g_overlayMaterial.object != nullptr) {
            FilledBox(f, s.fillColor, lo, hi);
            Render(f, g_overlayMaterial);
        }
        Box(f, s.line, lo, hi);
        Render(f, g_boxMaterial);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Algo no era como esperábamos: no dibujar este frame (mejor que crashear).
    }
}

void __fastcall RenderOutlineDetour(void* lrp, void* screenContext, void* block, void* region, const int* pos) {
    hooks::ActiveCallGuard guard;
    const OutlineSettings s = CopyOutline();
    if (!g_ready.load(std::memory_order_relaxed) || !s.enabled || pos == nullptr || !EnsureMaterials()) {
        g_renderOutline(lrp, screenContext, block, region, pos);
        return;
    }
    DrawOutline(lrp, screenContext, block, region, pos, s);
}

void DrawHitboxesSafe(std::uintptr_t levelRenderer, std::uintptr_t screenContext, const HitboxSettings& s) {
    __try {
        Frame f;
        if (MakeFrame(levelRenderer, screenContext, f)) {
            DrawHitboxes(f, s);
            Render(f, g_boxMaterial);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // No crashear: si algo falla, este frame no hay hitboxes.
    }
}

}  // namespace

bool Init() {
    const ScanResult begin = FindPattern(kTessBegin);
    const ScanResult vertex = FindPattern(kTessVertex);
    const ScanResult mesh = FindPattern(kRenderMesh);
    const ScanResult group = FindPattern(kMaterialGroupRef);
    if (begin.matches != 1 || vertex.matches != 1 || mesh.matches != 1 || group.matches != 1) {
        log::Warn("SDK: no se encontraron las funciones para dibujar en el mundo ({} {} {} {}).", begin.matches,
                  vertex.matches, mesh.matches, group.matches);
        return false;
    }
    g_begin = reinterpret_cast<TessBeginFn>(begin.address);
    g_vertex = reinterpret_cast<TessVertexFn>(vertex.address);
    g_renderMesh = reinterpret_cast<RenderMeshFn>(mesh.address);
    g_materialGroup = ResolveRipRelative(group.address + 3, group.address + 7);
    log::Info("SDK: dibujo en el mundo listo (Tessellator juego+0x{:X}, mallas juego+0x{:X}, materiales juego+0x{:X}).",
              begin.address - GameBase(), mesh.address - GameBase(), g_materialGroup - GameBase());

    const ScanResult outline = FindPattern(kRenderOutline);
    if (outline.matches == 1) {
        void* target = reinterpret_cast<void*>(outline.address);
        if (hooks::Create(target, reinterpret_cast<void*>(&RenderOutlineDetour), reinterpret_cast<void**>(&g_renderOutline),
                          "juego: contorno del bloque (renderOutlineSelection)") &&
            hooks::Enable(target, "juego: contorno del bloque (renderOutlineSelection)")) {
            g_outlineHooked.store(true);
        }
    }
    g_ready.store(true);
    return true;
}

bool Available() {
    return g_ready.load(std::memory_order_relaxed);
}

void SetOutline(const OutlineSettings& settings) {
    std::lock_guard lock(g_settingsMutex);
    g_outline = settings;
}

void SetHitboxes(const HitboxSettings& settings) {
    std::lock_guard lock(g_settingsMutex);
    g_hitboxes = settings;
}

void OnRenderLevel(std::uintptr_t levelRenderer, std::uintptr_t screenContext) {
    if (!g_ready.load(std::memory_order_relaxed)) {
        return;
    }
    const HitboxSettings hitboxes = CopyHitboxes();
    if (!hitboxes.enabled || !EnsureMaterials()) {
        return;
    }
    DrawHitboxesSafe(levelRenderer, screenContext, hitboxes);
}

}  // namespace luma::sdk::draw3d
