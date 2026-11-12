// =============================================================================
//  draw3d.cpp - Dibujo dentro del mundo (ver draw3d.h)
// =============================================================================
#include "sdk/draw3d.h"

#include "sdk/blocks.h"
#include "sdk/radar.h"
#include "render/rainbow.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
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

// La función que, CADA frame, mira el bloque apuntado y (si es sólido) llama a
// renderOutlineSelection: (LevelRendererPlayer, pantalla, BlockSource, posición, bool).
constexpr const char* kOutlineCaller =
    "41 57 41 56 56 57 55 53 48 83 EC 48 4D 89 CE 4C 89 C6 48 89 D7 48 89 CB 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 40 "
    "49 8B 00 48 8B 40 10";

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
constexpr int kPrimitiveTriangles = 2;  // lista de triángulos

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
using RenderMeshFn = void(__fastcall*)(void* screenContext, void* tess, void* material, void* params, void* a5, void* a6);
using RenderOutlineFn = void(__fastcall*)(void* lrp, void* screenContext, void* block, void* region, const int* pos);
using CreateMaterialFn = void(__fastcall*)(void* group, void* outSharedPtr, const void* hashedName);

TessBeginFn g_begin = nullptr;
TessVertexFn g_vertex = nullptr;
RenderMeshFn g_renderMesh = nullptr;
RenderOutlineFn g_renderOutline = nullptr;
std::uintptr_t g_materialGroup = 0;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_outlineHooked{false};
// Mientras el juego dibuja su contorno (en este hilo), para teñir SU malla.
thread_local bool t_inOutline = false;  // el juego está dibujando su contorno (en este hilo)

// Los materiales que pedimos al juego. Guardamos el "shared_ptr" crudo (dos
// punteros) y nunca lo liberamos: es un objeto del juego que vive mientras
// el juego viva.
struct RawSharedPtr {
    void* object = nullptr;
    void* control = nullptr;
};
RawSharedPtr g_boxMaterial;
// Materiales candidatos para el RELLENO del contorno. No sabemos con certeza
// cuál dibuja bien caras de un color sin textura en esta versión, así que se
// puede elegir entre varios (OutlineSettings::fillMaterial).
constexpr const char* kFillMaterials[kFillMaterialCount] = {"selection_overlay", "name_tag_depth_tested",
                                                            "fullscreen_cube_overlay_blend", "selection_box"};
RawSharedPtr g_fillMaterials[kFillMaterialCount];
bool g_fillTried[kFillMaterialCount]{};

std::mutex g_settingsMutex;
OutlineSettings g_outline;
HitboxSettings g_hitboxes;
ChunkBorderSettings g_chunks;
std::vector<WorldShapes> g_shapes[kChannelCount];  // other modules' shapes, by channel (SetWorldShapes)
int g_shapesMaterial = 0;
std::vector<WorldShapes> g_shapesFrame;  // the copy being drawn this frame (render thread only)

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
        log::Info("SDK: material del mundo selection_box: {}.", g_boxMaterial.object != nullptr ? "sí" : "no");
    }
    return g_boxMaterial.object != nullptr;
}

// El material de relleno elegido (se pide al juego la primera vez que se usa).
void* FillMaterial(int index) {
    if (index < 0 || index >= kFillMaterialCount) {
        index = 0;
    }
    if (!g_fillTried[index]) {
        g_fillTried[index] = true;
        g_fillMaterials[index] = CreateMaterial(kFillMaterials[index]);
        log::Info("SDK: material de relleno {}: {}.", kFillMaterials[index],
                  g_fillMaterials[index].object != nullptr ? "sí" : "no");
    }
    return g_fillMaterials[index].object;
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
    // Cada cara = 2 triángulos, y cada triángulo de los dos lados (para que
    // se vea desde adentro y desde afuera): 6 caras x 4 triángulos x 3 vértices.
    // (Antes usábamos "cuadrados", pero no se veían: triángulos es lo más básico.)
    g_begin(reinterpret_cast<void*>(f.tess), 0, kPrimitiveTriangles, 72, false);
    SetColor(f, c);
    constexpr int kOrder[12] = {0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0};
    for (const auto& face : faces) {
        for (int i : kOrder) g_vertex(reinterpret_cast<void*>(f.tess), face[i][0], face[i][1], face[i][2]);
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
    alignas(16) std::uint8_t params[0x80]{};
    g_renderMesh(reinterpret_cast<void*>(f.sc), reinterpret_cast<void*>(f.tess), material.object, params, nullptr, nullptr);
}

// --- Hitboxes: recorrer todas las cajas de entidades --------------------------------
//  1. Recorremos la "piscina" de cajas (AABBShapeComponent) del sistema de
//     entidades. Cada lugar tiene el número de la entidad dueña.
//  2. Descartamos lugares que ya no son de nadie: EnTT, al borrar, a veces
//     deja una "lápida" (los bits de versión en 1) o el lugar ya no coincide
//     con lo que dice el índice de la entidad. También pedimos que la entidad
//     tenga TIPO y POSICIÓN: un ítem que agarraste pierde esos componentes
//     antes que su caja (por eso quedaba la caja "fantasma").
//  3. Tu propia caja se reconoce por TU número de entidad (Actor +0x18).
//  4. Interpolación: el juego mueve las entidades 20 veces por segundo (cada
//     "tick"), pero las DIBUJA en cada frame en un punto intermedio entre la
//     posición del tick anterior y la del actual. La caja, en cambio, está en
//     la del tick actual: por eso se veía "adelantada" y a saltos. Corremos la
//     caja al mismo punto intermedio que usa el juego.
//
//  Las cajas se juntan en una lista y se dibujan agrupadas por color (una
//  llamada al juego por color: el color lo da la "tinta" de la pantalla).
std::atomic<int> g_hitboxLogs{0};
int g_nearLogs = 0;
int g_lastDrawn = 0;

constexpr std::uint32_t kStateVectorHash = 0x1B5D5238;
constexpr std::uintptr_t kStatePosOld = 0x0C;
constexpr std::uint32_t kVersionShift = 18;  // los bits de arriba del número de entidad
constexpr std::uint32_t kTombstoneVersion = 0xFFFFFFFFu >> kVersionShift;

enum Group { kGroupPlayer = 0, kGroupMob = 1, kGroupItem = 2, kGroupCount = 3 };

struct PendingBox {
    float lo[3];
    float hi[3];
    int group;
};
constexpr int kMaxBoxes = 256;
PendingBox g_pending[kMaxBoxes];  // solo lo usa el hilo de render (sin asignar memoria)
int g_pendingCount = 0;

// Tamaño real de StateVectorComponent en la piscina: lo comprobamos con tu
// jugador (su puntero en Actor +0x240 tiene que caer donde dice EnTT).
std::uintptr_t g_stateSize = 0;

std::uintptr_t DetectStateSize(std::uintptr_t player, std::uintptr_t states, std::uint32_t ownId) {
    std::uintptr_t own = 0;
    if (!SafeRead(player + offsets::kActorStateVector, own) || !LooksLikeDataPointer(own)) {
        return 0;
    }
    for (std::uintptr_t size : {std::uintptr_t{0x24}, std::uintptr_t{0x28}, std::uintptr_t{0x30}, std::uintptr_t{0x20}}) {
        if (entity::ComponentOf(states, ownId, size) == own) {
            return size;
        }
    }
    return 0;
}

// Si tu jugador no está en esta lista (en un mundo local puede ser la del
// servidor interno), lo comprobamos con las propias entidades: con el tamaño
// correcto, la posición de cada una cae DENTRO de su caja (de lado).
std::uintptr_t DetectStateSizeFromBoxes(std::uintptr_t boxes, std::uintptr_t states, const std::uintptr_t* entities,
                                        std::uintptr_t pages, std::size_t boxSize) {
    std::uintptr_t best = 0;
    int bestHits = 0;
    for (std::uintptr_t size : {std::uintptr_t{0x24}, std::uintptr_t{0x28}, std::uintptr_t{0x30}, std::uintptr_t{0x20}}) {
        int hits = 0, tried = 0;
        const std::size_t count = std::min<std::size_t>((entities[1] - entities[0]) / 4, 512);
        for (std::size_t i = 0; i < count && tried < 24; ++i) {
            std::uint32_t entity = 0;
            if (!SafeRead(entities[0] + i * 4, entity) || (entity & offsets::kEntityMask) == offsets::kEntityMask) continue;
            std::uintptr_t page = 0;
            if (!SafeRead(pages + (i / offsets::kPayloadPage) * 8, page) || !LooksLikePointer(page)) continue;
            const std::uintptr_t comp = page + (i % offsets::kPayloadPage) * boxSize;
            if (entity::ComponentOf(boxes, entity, boxSize) != comp) continue;
            float box[6]{};
            const std::uintptr_t state = entity::ComponentOf(states, entity, size);
            float pos[3]{};
            if (!SafeRead(comp, box) || state == 0 || !SafeRead(state, pos)) continue;
            ++tried;
            if (pos[0] >= box[0] - 0.05f && pos[0] <= box[3] + 0.05f && pos[2] >= box[2] - 0.05f && pos[2] <= box[5] + 0.05f) ++hits;
        }
        if (tried >= 3 && hits * 4 >= tried * 3 && hits > bestHits) {  // 3 de cada 4, por lo menos
            best = size;
            bestHits = hits;
        }
    }
    return best;
}

// En qué punto entre dos ticks estamos (0 = recién empezó el tick, 1 = el siguiente).
// Cuando cambia el contador de ticks de tu jugador anotamos la hora; después,
// cuánto pasó de los 50 ms que dura un tick.
float PartialTick(std::uintptr_t player) {
    static std::int32_t lastTicks = -1;
    static LARGE_INTEGER tickStart{};
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    std::int32_t ticks = 0;
    if (!SafeRead(player + offsets::kActorTicksExisted, ticks)) {
        return 1.0f;
    }
    if (ticks != lastTicks) {
        lastTicks = ticks;
        tickStart = now;
    }
    const double elapsed = static_cast<double>(now.QuadPart - tickStart.QuadPart) / static_cast<double>(frequency.QuadPart);
    return static_cast<float>(std::clamp(elapsed / 0.05, 0.0, 1.0));
}

// Junta en g_pending las cajas a dibujar (no dibuja nada todavía).
void CollectHitboxes(const Frame& f, const HitboxSettings& s, float* outLo, float* outHi) {
    g_lastDrawn = 0;
    g_pendingCount = 0;
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t registry = 0;
    std::uint32_t ownId = 0;
    if (player == 0 || !SafeRead(player + offsets::kActorEnttRegistry, registry) || !LooksLikePointer(registry) ||
        !SafeRead(player + offsets::kActorEntityId, ownId)) {
        return;
    }
    const std::uintptr_t boxes = entity::Storage(registry, kAabbHash);
    const std::uintptr_t types = entity::Storage(registry, kActorTypeHash);
    const std::uintptr_t states = entity::Storage(registry, kStateVectorHash);
    std::uintptr_t entities[2]{};  // el vector de entidades de la piscina de cajas
    std::uintptr_t pages = 0;
    if (boxes == 0 || !SafeRead(boxes + 0x20, entities) || !SafeRead(boxes + offsets::kStoragePayload, pages) ||
        entities[1] < entities[0]) {
        return;
    }
    if (g_stateSize == 0 && states != 0) {
        // (reintentar cada 2 segundos, no en cada frame)
        static ULONGLONG lastTry = 0;
        static bool logged = false;
        if (GetTickCount64() - lastTry >= 2000) {
            lastTry = GetTickCount64();
            g_stateSize = DetectStateSize(player, states, ownId);
            if (g_stateSize == 0) g_stateSize = DetectStateSizeFromBoxes(boxes, states, entities, pages, kAabbComponentSize);
            if (g_stateSize != 0 || !logged) {
                logged = true;
                log::Info("SDK: hitboxes: tamaño de la posición de las entidades: 0x{:X}.", g_stateSize);
            }
        }
    }
    const float alpha = PartialTick(player);
    // Dónde estás tú. En un mundo local el juego tiene DOS copias de tu
    // jugador (la del cliente y la del servidor interno), con números de
    // entidad distintos: además del número, salteamos cualquier jugador que
    // esté justo donde estás.
    float ownFeet[3]{};
    std::uintptr_t ownAabb = 0;
    const bool haveOwn = SafeRead(player + offsets::kActorAabb, ownAabb) && LooksLikeDataPointer(ownAabb) &&
                         SafeRead(ownAabb, ownFeet) && std::isfinite(ownFeet[0]);
    const std::size_t count = std::min<std::size_t>((entities[1] - entities[0]) / 4, 8192);
    const float maxDist2 = s.maxDistance * s.maxDistance;
    int skippedDead = 0;
    for (std::size_t i = 0; i < count && g_pendingCount < kMaxBoxes; ++i) {
        std::uint32_t entity = 0;
        if (!SafeRead(entities[0] + i * 4, entity) || (entity & offsets::kEntityMask) == offsets::kEntityMask ||
            (entity >> kVersionShift) == kTombstoneVersion) {
            continue;  // lugar vacío o "lápida" de una entidad borrada
        }
        const bool isSelf = (entity & offsets::kEntityMask) == (ownId & offsets::kEntityMask);
        if (isSelf && !s.showSelf) {
            continue;
        }
        std::uintptr_t page = 0;
        if (!SafeRead(pages + (i / offsets::kPayloadPage) * 8, page) || !LooksLikePointer(page)) {
            continue;
        }
        const std::uintptr_t comp = page + (i % offsets::kPayloadPage) * kAabbComponentSize;
        // El índice de la entidad tiene que apuntar a ESTE lugar (si no, es viejo).
        if (entity::ComponentOf(boxes, entity, kAabbComponentSize) != comp) {
            ++skippedDead;
            continue;
        }
        // Tipo de entidad: sin tipo, no es una entidad "viva" del mundo.
        const std::uintptr_t typeComp = entity::ComponentOf(types, entity, 4);
        std::uint32_t type = 0;
        if (typeComp == 0 || !SafeRead(typeComp, type)) {
            ++skippedDead;
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
        const float rawMin[3] = {box[0], box[1], box[2]};
        // Diagnóstico: la caja más cercana a ti (las primeras veces).
        if (haveOwn && g_nearLogs < 6 && std::fabs(box[0] - ownFeet[0]) < 3.0f && std::fabs(box[1] - ownFeet[1]) < 3.0f &&
            std::fabs(box[2] - ownFeet[2]) < 3.0f) {
            ++g_nearLogs;
            log::Info("SDK: caja cerca de ti: entidad 0x{:X} (tú 0x{:X}), min {:.2f} {:.2f} {:.2f}, tamaño {:.2f} x {:.2f}; "
                      "tus pies {:.2f} {:.2f} {:.2f}; cámara {:.2f} {:.2f} {:.2f}",
                      entity, ownId, box[0], box[1], box[2], width, height, ownFeet[0], ownFeet[1], ownFeet[2],
                      f.origin[0], f.origin[1], f.origin[2]);
        }
        float motion[3]{};  // cuánto se movió en el último tick (por eje)
        // Interpolación con la posición (actual y del tick anterior).
        if (g_stateSize != 0) {
            const std::uintptr_t state = entity::ComponentOf(states, entity, g_stateSize);
            float pos[6]{};
            if (state == 0 || !SafeRead(state, pos)) {
                ++skippedDead;
                continue;  // sin posición: tampoco está viva
            }
            float delta[3]{};
            for (int k = 0; k < 3; ++k) {
                motion[k] = std::isfinite(pos[k] - pos[3 + k]) ? std::fabs(pos[k] - pos[3 + k]) : 0.0f;
            }
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                delta[k] = (pos[3 + k] - pos[k]) * (1.0f - alpha);  // de "ahora" hacia "antes"
                ok &= std::isfinite(delta[k]) && std::fabs(delta[k]) < 4.0f;  // un teletransporte no se interpola
            }
            if (ok) {
                for (int k = 0; k < 3; ++k) {
                    box[k] += delta[k];
                    box[k + 3] += delta[k];
                }
            }
        }
        // En primera persona la cámara está DENTRO de tu caja: ninguna caja
        // que contenga la cámara se dibuja (ni con "Tu propia caja": esa
        // opción es para tercera persona, donde la cámara está afuera). El
        // margen incluye lo que se movió en el último tick: al caer rápido la
        // cámara (suavizada) y la caja (a saltos) se separan un poco.
        {
            bool inside = true;
            for (int k = 0; k < 3; ++k) {
                // (en altura, varios ticks de margen: la copia del servidor
                // de un mundo local va atrasada cuando caes)
                const float margin = 0.3f + motion[k] * (k == 1 ? 3.0f : 1.0f);
                inside &= f.origin[k] > box[k] - margin && f.origin[k] < box[k + 3] + margin;
            }
            if (inside) {
                continue;
            }
        }
        const float cx = (box[0] + box[3]) * 0.5f - f.origin[0];
        const float cy = (box[1] + box[4]) * 0.5f - f.origin[1];
        const float cz = (box[2] + box[5]) * 0.5f - f.origin[2];
        if (cx * cx + cy * cy + cz * cz > maxDist2) {
            continue;
        }
        const std::uint32_t kind = type & 0xFFFF;
        // Tu otra copia (la del servidor interno de un mundo local). Al caer va
        // unos ticks atrasada: en altura la tolerancia crece con la velocidad
        // de caída (de lado sigue siendo estricta, para no esconder a otros).
        if (!s.showSelf && haveOwn && std::fabs(rawMin[0] - ownFeet[0]) < 0.75f + motion[0] &&
            std::fabs(rawMin[1] - ownFeet[1]) < 1.0f + motion[1] * 4.0f && std::fabs(rawMin[2] - ownFeet[2]) < 0.75f + motion[2]) {
            continue;
        }
        const bool isPlayer = kind == kTypePlayer || isSelf;
        const bool isItem = kind == kTypeItem || kind == kTypeXpOrb;
        const int group = isPlayer ? kGroupPlayer : (isItem ? kGroupItem : kGroupMob);
        const bool wanted = group == kGroupPlayer ? s.players : (group == kGroupItem ? s.items : s.mobs);
        if (!wanted) {
            continue;
        }
        PendingBox& p = g_pending[g_pendingCount++];
        for (int k = 0; k < 3; ++k) {
            p.lo[k] = box[k];
            p.hi[k] = box[k + 3];
            outLo[k] = std::min(outLo[k], box[k]);
            outHi[k] = std::max(outHi[k], box[k + 3]);
        }
        p.group = group;
        ++g_lastDrawn;
    }
    if (g_hitboxLogs.fetch_add(1) < 3) {
        log::Info("SDK: hitboxes: {} cajas en la piscina, {} a dibujar, {} descartadas por no estar vivas.", count,
                  g_lastDrawn, skippedDead);
    }
}

// --- Dibujar DENTRO de las llamadas de mallas del propio juego ----------------------
//  Lo aprendimos del log (1.26.63): cuando el juego dibuja su contorno llama a
//  renderMeshImmediately(pantalla, tessellator, material, parámetros, extra).
//  "parámetros" lleva punteros a datos de ESA llamada y una caja (+0x20 mínimo,
//  +0x2C máximo, en coordenadas del mundo): lo que queda fuera de esa caja no
//  se dibuja. Por eso nuestras llamadas "sueltas" (con ceros) no mostraban
//  nada. Ahora, justo después de una llamada del juego, armamos lo nuestro y
//  llamamos otra vez con SUS parámetros (y la caja agrandada para que entre lo
//  nuestro) y SU material del contorno.
constexpr std::uintptr_t kParamsSize = 0x80;
constexpr std::uintptr_t kParamsBoundsMin = 0x20;
constexpr std::uintptr_t kParamsBoundsMax = 0x2C;

OutlineSettings CopyOutline() {
    std::lock_guard lock(g_settingsMutex);
    return g_outline;
}

HitboxSettings CopyHitboxes() {
    std::lock_guard lock(g_settingsMutex);
    return g_hitboxes;
}

ChunkBorderSettings CopyChunks() {
    std::lock_guard lock(g_settingsMutex);
    return g_chunks;
}

// ¿Hay algo para dibujar en el mundo además del contorno? (hitboxes, bordes de chunk, formas)
bool WorldWanted() {
    std::lock_guard lock(g_settingsMutex);
    bool shapes = false;
    for (const auto& channel : g_shapes) shapes |= !channel.empty();
    return g_hitboxes.enabled || g_chunks.enabled || shapes;
}

// Takes this frame's shapes (outside the __try: copying a vector isn't allowed in there).
void TakeShapesForFrame() {
    std::lock_guard lock(g_settingsMutex);
    g_shapesFrame.clear();
    for (const auto& channel : g_shapes) g_shapesFrame.insert(g_shapesFrame.end(), channel.begin(), channel.end());
}

int ShapesMaterial() {
    std::lock_guard lock(g_settingsMutex);
    return g_shapesMaterial;
}

void Grow(float* lo, float* hi, const float* a, const float* b);  // (below)

// The triangles of one group (both faces: seen from the front and the back).
void Triangles(const Frame& f, const WorldShapes& shape, float* lo, float* hi) {
    const std::size_t floats = shape.triangles.size() / 9 * 9;
    if (floats == 0) return;
    g_begin(reinterpret_cast<void*>(f.tess), 0, kPrimitiveTriangles, static_cast<int>(floats / 3 * 2), false);
    SetColor(f, Color{});
    const float* t = shape.triangles.data();
    for (std::size_t i = 0; i < floats; i += 9) {
        const float* a = t + i;
        const float* b = t + i + 3;
        const float* c = t + i + 6;
        for (const float* v : {a, b, c, a, c, b}) {
            g_vertex(reinterpret_cast<void*>(f.tess), v[0] - f.origin[0], v[1] - f.origin[1], v[2] - f.origin[2]);
            const float p[3] = {v[0], v[1], v[2]};
            Grow(lo, hi, p, p);
        }
    }
}

// --- Bordes de chunk (como el F3+G de Java) --------------------------------------------
//  Un chunk es una columna de 16x16 bloques. Dibujamos, alrededor de donde
//  estás (de "range" bloques abajo a "range" arriba):
//    - el chunk actual: líneas cada 2 bloques en sus 4 paredes (amarillo),
//    - sus 4 esquinas: columnas de arriba a abajo (azul),
//    - las esquinas de los 8 chunks vecinos (rojo).
//  Cada grupo de color es una llamada al juego (el color lo da la "tinta").
void Grow(float* lo, float* hi, const float* a, const float* b);  // (más abajo)

void ChunkLines(const Frame& f, const Color& c, const ChunkBorderSettings& cs, int group, float* lo, float* hi) {
    const float cx = std::floor(f.origin[0] / 16.0f) * 16.0f;
    const float cz = std::floor(f.origin[2] / 16.0f) * 16.0f;
    const float y0 = std::floor(f.origin[1]) - static_cast<float>(cs.range);
    const float y1 = std::floor(f.origin[1]) + static_cast<float>(cs.range);
    auto line = [&](float ax, float ay, float az, float bx, float by, float bz) {
        Line(f, c, ax, ay, az, bx, by, bz);
        const float a[3] = {std::min(ax, bx), std::min(ay, by), std::min(az, bz)};
        const float b[3] = {std::max(ax, bx), std::max(ay, by), std::max(az, bz)};
        Grow(lo, hi, a, b);
    };
    if (group == 0) {  // paredes del chunk actual
        for (float t = 2.0f; t < 16.0f; t += 2.0f) {
            line(cx + t, y0, cz, cx + t, y1, cz);
            line(cx + t, y0, cz + 16, cx + t, y1, cz + 16);
            line(cx, y0, cz + t, cx, y1, cz + t);
            line(cx + 16, y0, cz + t, cx + 16, y1, cz + t);
        }
        for (float y = y0; y <= y1; y += 2.0f) {
            line(cx, y, cz, cx + 16, y, cz);
            line(cx, y, cz + 16, cx + 16, y, cz + 16);
            line(cx, y, cz, cx, y, cz + 16);
            line(cx + 16, y, cz, cx + 16, y, cz + 16);
        }
    } else if (group == 1) {  // esquinas del chunk actual
        for (int i = 0; i < 4; ++i) {
            const float x = cx + ((i & 1) ? 16.0f : 0.0f);
            const float z = cz + ((i & 2) ? 16.0f : 0.0f);
            line(x, y0 - 32.0f, z, x, y1 + 32.0f, z);
        }
    } else if (cs.neighbors) {  // esquinas de los vecinos
        for (int ix = -1; ix <= 2; ++ix) {
            for (int iz = -1; iz <= 2; ++iz) {
                if ((ix == 0 || ix == 1) && (iz == 0 || iz == 1)) continue;  // esas son las del chunk actual
                const float x = cx + ix * 16.0f;
                const float z = cz + iz * 16.0f;
                line(x, y0, z, x, y1, z);
            }
        }
    }
}

thread_local bool t_inLevel = false;          // dentro de renderLevel
thread_local bool t_levelDrawn = false;       // ¿ya dibujamos los hitboxes en este frame?
thread_local int t_levelAttempts = 0;
thread_local const int* t_outlinePos = nullptr;
thread_local std::uintptr_t t_lrp = 0;        // LevelRendererPlayer del contorno (tiene el origen)
thread_local std::uintptr_t t_levelLrp = 0;   // el de renderLevel
thread_local bool t_outlineDrawn = false;
thread_local bool t_synthetic = false;        // este contorno lo pedimos nosotros (invisible)
thread_local bool t_outlineHappened = false;  // ¿el juego llegó a dibujar un contorno?
void* g_lastBlock = nullptr;                  // el último bloque que miraste (hilo de render)
std::uintptr_t g_lastBlockVtable = 0;         // para comprobar que sigue siendo un bloque
void* g_gameMaterial = nullptr;               // el material que usa el juego para su contorno
std::atomic<int> g_ownLogs{0};
std::atomic<int> g_shapeLogs{0};

void Grow(float* lo, float* hi, const float* a, const float* b) {
    for (int i = 0; i < 3; ++i) {
        lo[i] = std::min(lo[i], a[i]);
        hi[i] = std::max(hi[i], b[i]);
    }
}

// ¿Los parámetros tienen la forma que conocemos? (la caja con números razonables)
bool LooksLikeParams(void* params) {
    float box[6]{};
    if (!LooksLikePointer(reinterpret_cast<std::uintptr_t>(params)) ||
        !SafeRead(reinterpret_cast<std::uintptr_t>(params) + kParamsBoundsMin, box)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(box[i]) || !std::isfinite(box[i + 3]) || box[i] > box[i + 3] || std::fabs(box[i]) > 3.0e7f) {
            return false;
        }
    }
    return true;
}


// Arma y dibuja lo nuestro con los parámetros de una llamada del juego.
// (Función aparte para el __try: no puede tener objetos con destructor.)
//
// EL COLOR: el juego tiñe todo lo que dibuja con un color de la pantalla
// (ScreenContext +0x30). Dentro de su contorno ese color es el del contorno
// (oscuro y casi transparente): por eso el relleno no se veía. Ahora, en cada
// llamada nuestra, ponemos ESE color en el que queremos (los vértices van en
// blanco) y al final devolvemos el del juego.
// Si "rainbow" está activo, el color del arcoíris de ahora (con la
// transparencia del color elegido). "offset" corre el tono (cada grupo un poco
// distinto, para que se distingan).
Color Rainbowed(const Color& c, bool on, float offset) {
    if (!on) return c;
    Color out = c;
    rainbow::Color(rainbow::kDefaultSpeed, offset, out.r, out.g, out.b);
    return out;
}

void Tint(bool haveColor, std::uintptr_t colorPtr, const Color& c) {
    if (haveColor) {
        const float v[4] = {c.r, c.g, c.b, c.a};
        SafeWrite(colorPtr, v, sizeof(v));
    }
}

// Pone la caja de "lo que se dibuja" (con un margen) y llama al juego.
void Submit(void* sc, void* tess, void* material, std::uint8_t* copy, void* a5, void* a6, float* lo, float* hi) {
    for (int i = 0; i < 3; ++i) {
        lo[i] -= 1.0f;
        hi[i] += 1.0f;
    }
    std::memcpy(copy + kParamsBoundsMin, lo, sizeof(float) * 3);
    std::memcpy(copy + kParamsBoundsMax, hi, sizeof(float) * 3);
    g_renderMesh(sc, tess, material, copy, a5, a6);
}

// "closeIfEmpty": en el contorno invisible, si no hay nada nuestro que
// dibujar, igual hay que CERRAR el lote de líneas del juego con una llamada de
// dibujo (si no, queda a medio armar y lo siguiente que lo usa, como la mira
// del juego, no se dibuja). Para eso, una línea transparente muy abajo.
void DrawOwnSafe(void* sc, void* tess, void* material, void* params, void* a5, void* a6, std::uintptr_t lrp,
                 const OutlineSettings& o, const HitboxSettings& h, const int* blockPos, bool hitboxes,
                 void* fillMaterial, bool closeIfEmpty = false, void* shapesMaterial = nullptr) {
    std::uintptr_t colorPtr = 0;
    float saved[4]{};
    bool haveColor = false;
    __try {
        Frame f;
        f.sc = reinterpret_cast<std::uintptr_t>(sc);
        f.tess = reinterpret_cast<std::uintptr_t>(tess);
        if (!SafeRead(lrp + kCameraOrigin, f.origin) || !std::isfinite(f.origin[0])) {
            return;
        }
        alignas(16) std::uint8_t copy[kParamsSize];
        if (!SafeCopy(reinterpret_cast<std::uintptr_t>(params), copy, sizeof(copy))) {
            return;
        }
        haveColor = SafeRead(f.sc + kScreenContextColor, colorPtr) && LooksLikePointer(colorPtr) &&
                    SafeCopy(colorPtr, saved, sizeof(saved));
        const Color white{};
        // 1. Relleno del contorno.
        if (blockPos != nullptr && o.fill && fillMaterial != nullptr) {
            float blo[3]{};
            float bhi[3]{};
            for (int i = 0; i < 3; ++i) {
                blo[i] = static_cast<float>(blockPos[i]) - 0.004f;
                bhi[i] = static_cast<float>(blockPos[i]) + 1.004f;
            }
            const Color fillColor = Rainbowed(o.fillColor, o.rainbow, 0.0f);
            FilledBox(f, haveColor ? white : fillColor, blo, bhi);
            Tint(haveColor, colorPtr, fillColor);
            Submit(sc, tess, fillMaterial, copy, a5, a6, blo, bhi);
        }
        // 2. Bordes de chunk.
        int drawn = 0;
        const ChunkBorderSettings cs = CopyChunks();
        if (hitboxes && cs.enabled) {
            const Color* colors[3] = {&cs.inner, &cs.corner, &cs.neighbor};
            for (int g = 0; g < 3; ++g) {
                float lo[3] = {3.0e7f, 3.0e7f, 3.0e7f};
                float hi[3] = {-3.0e7f, -3.0e7f, -3.0e7f};
                const Color color = Rainbowed(*colors[g], cs.rainbow, g * 0.33f);
                ChunkLines(f, haveColor ? white : color, cs, g, lo, hi);
                if (lo[0] <= hi[0]) {
                    Tint(haveColor, colorPtr, color);
                    Submit(sc, tess, material, copy, a5, a6, lo, hi);
                    ++drawn;
                }
            }
        }
        // 3. Hitboxes: una llamada por color.
        if (hitboxes && h.enabled) {
            float hlo[3] = {3.0e7f, 3.0e7f, 3.0e7f};
            float hhi[3] = {-3.0e7f, -3.0e7f, -3.0e7f};
            CollectHitboxes(f, h, hlo, hhi);
            const Color* colors[kGroupCount] = {&h.playerColor, &h.mobColor, &h.itemColor};
            for (int g = 0; g < kGroupCount; ++g) {
                float lo[3] = {3.0e7f, 3.0e7f, 3.0e7f};
                float hi[3] = {-3.0e7f, -3.0e7f, -3.0e7f};
                int n = 0;
                for (int i = 0; i < g_pendingCount; ++i) {
                    if (g_pending[i].group != g) {
                        continue;
                    }
                    Box(f, haveColor ? white : Rainbowed(*colors[g], h.rainbow, g * 0.33f), g_pending[i].lo, g_pending[i].hi);
                    Grow(lo, hi, g_pending[i].lo, g_pending[i].hi);
                    ++n;
                }
                if (n > 0) {
                    Tint(haveColor, colorPtr, Rainbowed(*colors[g], h.rainbow, g * 0.33f));
                    Submit(sc, tess, material, copy, a5, a6, lo, hi);
                    drawn += n;
                }
            }
        }
        // 4. Shapes from other modules (Lyric Flow), one call per color.
        if (hitboxes) {
            for (const WorldShapes& shape : g_shapesFrame) {
                float lo[3] = {3.0e7f, 3.0e7f, 3.0e7f};
                float hi[3] = {-3.0e7f, -3.0e7f, -3.0e7f};
                void* useMaterial = shape.lines ? material : shapesMaterial;
                if (useMaterial == nullptr) continue;
                if (shape.lines) {
                    const float* p = shape.triangles.data();
                    for (std::size_t i = 0; i + 6 <= shape.triangles.size(); i += 6) {
                        Line(f, haveColor ? white : shape.color, p[i], p[i + 1], p[i + 2], p[i + 3], p[i + 4], p[i + 5]);
                        const float a[3] = {std::min(p[i], p[i + 3]), std::min(p[i + 1], p[i + 4]), std::min(p[i + 2], p[i + 5])};
                        const float b[3] = {std::max(p[i], p[i + 3]), std::max(p[i + 1], p[i + 4]), std::max(p[i + 2], p[i + 5])};
                        Grow(lo, hi, a, b);
                    }
                } else {
                    Triangles(f, shape, lo, hi);
                }
                if (g_shapeLogs.fetch_add(1) < 2) {
                    log::Info("SDK: world shapes: {} triangles, color a={:.2f}, box {:.1f} {:.1f} {:.1f} .. {:.1f} {:.1f} {:.1f}, "
                              "camera {:.1f} {:.1f} {:.1f}, material 0x{:X}.",
                              shape.triangles.size() / (shape.lines ? 6 : 9), shape.color.a, lo[0], lo[1], lo[2], hi[0], hi[1],
                              hi[2], f.origin[0], f.origin[1], f.origin[2], reinterpret_cast<std::uintptr_t>(useMaterial));
                }
                if (lo[0] <= hi[0]) {
                    Tint(haveColor, colorPtr, shape.color);
                    Submit(sc, tess, useMaterial, copy, a5, a6, lo, hi);
                    ++drawn;
                }
            }
        }
        if (drawn == 0 && closeIfEmpty) {
            float lo[3] = {f.origin[0] - 0.1f, f.origin[1] - 4000.0f, f.origin[2] - 0.1f};
            float hi[3] = {f.origin[0] + 0.1f, f.origin[1] - 3999.9f, f.origin[2] + 0.1f};
            const Color clear{1.0f, 1.0f, 1.0f, 0.0f};
            Box(f, clear, lo, hi);
            Tint(haveColor, colorPtr, clear);
            Submit(sc, tess, material, copy, a5, a6, lo, hi);
        }
        if (haveColor) {
            SafeWrite(colorPtr, saved, sizeof(saved));
        }
        if (drawn > 0 && g_ownLogs.fetch_add(1) < 2) {
            log::Info("SDK: dibujado dentro de una llamada del juego: {} cajas.", drawn);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Algo no era como esperábamos: este frame no se dibuja (mejor que crashear).
        if (haveColor) {
            SafeWrite(colorPtr, saved, sizeof(saved));
        }
    }
}

std::atomic<int> g_meshLogs{0};
// Diagnóstico: por qué camino se dibujaron los hitboxes (se loguea cada tanto).
std::atomic<int> g_outlinePathDraws{0};
std::atomic<int> g_levelPathDraws{0};

// El color con que el juego tiñe lo que dibuja (ScreenContext +0x30 -> float[4]).
// Dentro de su contorno, el juego lo fija justo antes de esta llamada: lo
// cambiamos acá por el nuestro y lo devolvemos después.
constexpr std::uintptr_t kTessVectors[] = {0x10, 0x70};  // posiciones, colores

bool ClearTessellator(void* tess) {
    const auto t = reinterpret_cast<std::uintptr_t>(tess);
    for (std::uintptr_t off : kTessVectors) {
        std::uintptr_t vec[3]{};  // inicio, fin, capacidad
        if (!SafeRead(t + off, vec) || vec[1] < vec[0] || vec[2] < vec[1] || (vec[0] != 0 && !LooksLikePointer(vec[0]))) {
            return false;  // no tiene la forma esperada: no tocar
        }
    }
    for (std::uintptr_t off : kTessVectors) {
        std::uintptr_t begin = 0;
        SafeRead(t + off, begin);
        SafeWrite(t + off + 8, &begin, sizeof(begin));
    }
    return true;
}

bool TintScreen(void* screenContext, const Color& c, float* saved, std::uintptr_t& colorPtr) {
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(screenContext) + kScreenContextColor, colorPtr) ||
        !LooksLikePointer(colorPtr) || !SafeCopy(colorPtr, saved, sizeof(float) * 4)) {
        return false;
    }
    const float tint[4] = {c.r, c.g, c.b, c.a};
    return SafeWrite(colorPtr, tint, sizeof(tint));
}

void __fastcall RenderMeshDetour(void* screenContext, void* tess, void* material, void* params, void* a5, void* a6) {
    hooks::ActiveCallGuard guard;
    if (t_inOutline && t_synthetic) {
        // Un contorno que pedimos NOSOTROS (ver OutlineCallerDetour): la malla
        // del juego no se dibuja (no queremos ver ese contorno), pero usamos
        // sus parámetros para dibujar los hitboxes.
        // IMPORTANTE: el juego ya cargó las líneas de SU contorno en el
        // Tessellator (las del último bloque que miraste). Si quedan ahí,
        // salen junto con lo nuestro. Las BORRAMOS sin dibujarlas: el
        // Tessellator guarda los vértices en dos listas (std::vector:
        // inicio, fin, capacidad) en +0x10 (posiciones) y +0x70 (colores);
        // poner "fin = inicio" las deja vacías sin liberar memoria.
        if (!ClearTessellator(tess)) {
            return;  // no se pudo: mejor no dibujar nada este frame
        }
        if (!t_outlineDrawn && g_ready.load(std::memory_order_relaxed) && LooksLikeParams(params)) {
            t_outlineDrawn = true;
            t_levelDrawn = true;
            g_levelPathDraws.fetch_add(1, std::memory_order_relaxed);
            TakeShapesForFrame();
            DrawOwnSafe(screenContext, tess, material, params, a5, a6, t_lrp, OutlineSettings{}, CopyHitboxes(), nullptr,
                        true, nullptr, /*closeIfEmpty=*/true, g_shapesFrame.empty() ? nullptr : FillMaterial(ShapesMaterial()));
        } else if (g_ready.load(std::memory_order_relaxed) && LooksLikeParams(params)) {
            // (ya dibujamos este frame: solo cerrar el lote vacío)
            DrawOwnSafe(screenContext, tess, material, params, a5, a6, t_lrp, OutlineSettings{}, HitboxSettings{}, nullptr,
                        false, nullptr, /*closeIfEmpty=*/true);
        }
        return;
    }
    // Primero, lo que el juego quería dibujar (si es su contorno y el módulo
    // está activo, con nuestro color).
    float saved[4]{};
    std::uintptr_t colorPtr = 0;
    bool tinted = false;
    if (t_inOutline && !t_outlineDrawn && g_ready.load(std::memory_order_relaxed)) {
        const OutlineSettings o = CopyOutline();
        if (o.enabled) {
            tinted = TintScreen(screenContext, Rainbowed(o.line, o.rainbow, 0.0f), saved, colorPtr);
        }
    }
    g_renderMesh(screenContext, tess, material, params, a5, a6);
    if (tinted) {
        SafeWrite(colorPtr, saved, sizeof(saved));
    }
    if (!g_ready.load(std::memory_order_relaxed)) {
        return;
    }
    if (t_inOutline && !t_outlineDrawn) {
        // La malla del contorno del juego: acá dibujamos el nuestro (y los
        // hitboxes, si en este frame todavía no se pudieron dibujar).
        t_outlineDrawn = true;
        g_gameMaterial = material;
        if (g_meshLogs.fetch_add(1) < 1) {
            log::Info("SDK: contorno del juego: material 0x{:X}, parámetros con caja.",
                      reinterpret_cast<std::uintptr_t>(material));
        }
        const OutlineSettings o = CopyOutline();
        const HitboxSettings h = CopyHitboxes();
        const bool hitboxes = WorldWanted() && !t_levelDrawn;  // hitboxes o bordes de chunk
        if (((o.enabled && o.fill) || hitboxes) && LooksLikeParams(params)) {
            if (hitboxes) TakeShapesForFrame();
            DrawOwnSafe(screenContext, tess, material, params, a5, a6, t_lrp, o, h, o.enabled ? t_outlinePos : nullptr,
                        hitboxes, o.fill ? FillMaterial(o.fillMaterial) : nullptr, false,
                        hitboxes && !g_shapesFrame.empty() ? FillMaterial(ShapesMaterial()) : nullptr);
            if (hitboxes) {
                t_levelDrawn = true;
                g_outlinePathDraws.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }
    // Sin mirar un bloque: ver OutlineCallerDetour (un contorno invisible).
}

// --- Hitboxes SIEMPRE, mires lo que mires --------------------------------------------
//  Nuestros hitboxes necesitan una llamada de dibujo del propio juego "del
//  mundo" (sus parámetros). En esta versión, la única así es la del contorno
//  del bloque que miras. Si miras al cielo, no hay contorno -> no había
//  hitboxes.
//
//  Solución: el juego llama CADA frame a una función que toma el bloque que
//  miras, comprueba si es sólido y, si lo es, dibuja su contorno. La
//  enganchamos: si en este frame NO dibujó ningún contorno, la volvemos a
//  llamar con un bloque sólido cercano (el último que miraste, o el que tienes
//  bajo los pies). Ese contorno es "invisible": RenderMeshDetour no deja que
//  el juego lo dibuje, solo usa sus parámetros para dibujar los hitboxes.
void __fastcall RenderOutlineDetour(void* lrp, void* screenContext, void* block, void* region, const int* pos);
using OutlineCallerFn = void(__fastcall*)(void* lrp, void* screenContext, void* region, const int* pos, bool flag);
OutlineCallerFn g_outlineCaller = nullptr;

std::atomic<int> g_callerCalls{0};  // diagnóstico: frames sin contorno en que lo intentamos

// El bloque en una posición: BlockSource::getBlock (función virtual +0x10, la
// misma que usa el juego en esta función). nullptr si algo falla.
using GetBlockFn = void*(__fastcall*)(void* region, const int* pos);
void* GetBlockSafe(void* region, const int* pos) {
    __try {
        std::uintptr_t vtable = 0;
        std::uintptr_t fn = 0;
        if (!SafeRead(reinterpret_cast<std::uintptr_t>(region), vtable) || !SafeRead(vtable + 0x10, fn) ||
            !LooksLikePointer(fn & ~std::uintptr_t{0xF})) {
            return nullptr;
        }
        return reinterpret_cast<GetBlockFn>(fn)(region, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Llamada protegida a renderOutlineSelection (aparte, por el __try).
void CallRenderOutlineSafe(void* lrp, void* screenContext, void* block, void* region, const int* pos) {
    __try {
        RenderOutlineDetour(lrp, screenContext, block, region, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall OutlineCallerDetour(void* lrp, void* screenContext, void* region, const int* pos, bool flag) {
    hooks::ActiveCallGuard guard;
    t_outlineHappened = false;
    g_outlineCaller(lrp, screenContext, region, pos, flag);
    // The minimap reads blocks here: the game's drawing thread and its BlockSource (sdk/blocks.h).
    blocks::OnRegion(region);
    radar::OnFrame();  // (the entities for the maps: local worlds only)
    if (t_outlineHappened || t_levelDrawn || !g_ready.load(std::memory_order_relaxed) || !WorldWanted()) {
        return;  // ya se dibujaron (junto a tu contorno) o no hacen falta
    }
    // Necesitamos un bloque SÓLIDO (para que el juego arme un contorno), pero
    // lo dibujamos 4000 bloques debajo de ti: fuera de lo que la cámara puede
    // ver, así ese contorno nunca aparece en pantalla. Candidatos: el último
    // bloque que miraste (los bloques del juego viven mientras el juego) o
    // los que tienes bajo los pies.
    int candidates[10][3];
    int count = 0;
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t aabb = 0;
    float feet[3]{};
    if (player != 0 && SafeRead(player + offsets::kActorAabb, aabb) && LooksLikeDataPointer(aabb) && SafeRead(aabb, feet) &&
        std::isfinite(feet[0]) && std::isfinite(feet[1]) && std::isfinite(feet[2])) {
        for (int dy = 1; dy <= 9 && count < 10; ++dy) {
            candidates[count][0] = static_cast<int>(std::floor(feet[0]));
            candidates[count][1] = static_cast<int>(std::floor(feet[1])) - dy;
            candidates[count][2] = static_cast<int>(std::floor(feet[2]));
            ++count;
        }
    }
    g_callerCalls.fetch_add(1, std::memory_order_relaxed);
    const int base[3] = {static_cast<int>(std::floor(feet[0])), static_cast<int>(std::floor(feet[1])),
                         static_cast<int>(std::floor(feet[2]))};
    const int farPos[3] = {base[0], base[1] - 4000, base[2]};
    t_synthetic = true;
    std::uintptr_t vtable = 0;
    if (g_lastBlock != nullptr && SafeRead(reinterpret_cast<std::uintptr_t>(g_lastBlock), vtable) &&
        vtable == g_lastBlockVtable) {
        CallRenderOutlineSafe(lrp, screenContext, g_lastBlock, region, farPos);
    }
    for (int i = 0; i < count && !t_outlineHappened; ++i) {
        void* block = GetBlockSafe(region, candidates[i]);
        if (block != nullptr) {
            CallRenderOutlineSafe(lrp, screenContext, block, region, farPos);
        }
    }
    t_synthetic = false;
    (void)flag;
}

std::atomic<int> g_outlineCalls{0};

void __fastcall RenderOutlineDetour(void* lrp, void* screenContext, void* block, void* region, const int* pos) {
    hooks::ActiveCallGuard guard;
    if (g_outlineCalls.fetch_add(1) < 1) {
        log::Info("SDK: el juego dibuja el contorno (bloque {}, {}, {}), llamado desde juego+0x{:X}.", pos ? pos[0] : 0,
                  pos ? pos[1] : 0, pos ? pos[2] : 0, reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - GameBase());
    }
    // Marcar que estamos dentro: el hook de mallas hace el resto.
    t_inOutline = true;
    t_outlineDrawn = false;
    t_outlineHappened = true;
    if (!t_synthetic && block != nullptr) {
        // El último bloque que miraste (para el contorno invisible).
        std::uintptr_t vtable = 0;
        if (SafeRead(reinterpret_cast<std::uintptr_t>(block), vtable)) {
            g_lastBlock = block;
            g_lastBlockVtable = vtable;
        }
    }
    t_outlinePos = pos;
    t_lrp = reinterpret_cast<std::uintptr_t>(lrp);
    g_renderOutline(lrp, screenContext, block, region, pos);
    t_inOutline = false;
}

}  // namespace

void EndLevel();
void CountFrame();

void BeginLevel(std::uintptr_t levelRenderer) {
    CountFrame();  // el frame anterior ya terminó: anotar si se dibujaron
    t_inLevel = true;
    t_levelDrawn = false;
    t_levelAttempts = 0;
    std::uintptr_t lrp = 0;
    t_levelLrp = SafeRead(levelRenderer + kLevelRendererPlayer, lrp) && LooksLikePointer(lrp) ? lrp : 0;
}

void EndLevel() {
    t_inLevel = false;
}

void CountFrame() {
    // Cada ~600 frames (las primeras 12 veces): cuántos frames dibujaron los
    // hitboxes y por qué camino. Sirve para saber qué pasa en un servidor.
    static int frames = 0;
    static int missed = 0;
    static int reports = 0;
    if (!t_levelDrawn) {
        ++missed;
    }
    if (++frames >= 600 && reports < 12) {
        if (CopyHitboxes().enabled) {
            ++reports;
            log::Info("SDK: hitboxes en los últimos {} frames: {} junto a tu contorno, {} con un contorno invisible, {} "
                      "sin dibujar ({} intentos de contorno invisible).",
                      frames, g_outlinePathDraws.exchange(0), g_levelPathDraws.exchange(0), missed,
                      g_callerCalls.exchange(0));
        }
        frames = 0;
        missed = 0;
    }
}

namespace {

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
    {
        void* target = reinterpret_cast<void*>(mesh.address);
        if (!hooks::Create(target, reinterpret_cast<void*>(&RenderMeshDetour), reinterpret_cast<void**>(&g_renderMesh),
                           "juego: dibujar mallas (renderMeshImmediately)") ||
            !hooks::Enable(target, "juego: dibujar mallas (renderMeshImmediately)")) {
            g_renderMesh = reinterpret_cast<RenderMeshFn>(mesh.address);  // sin hook: al menos podemos llamarla
        }
    }
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
    const ScanResult caller = FindPattern(kOutlineCaller);
    if (caller.matches == 1 && g_outlineHooked.load()) {
        void* target = reinterpret_cast<void*>(caller.address);
        if (!hooks::Create(target, reinterpret_cast<void*>(&OutlineCallerDetour), reinterpret_cast<void**>(&g_outlineCaller),
                           "juego: elegir el contorno del bloque") ||
            !hooks::Enable(target, "juego: elegir el contorno del bloque")) {
            g_outlineCaller = nullptr;
        }
    } else {
        log::Warn("SDK: no se encontró la función del contorno ({}): los hitboxes solo se verán mirando un bloque.",
                  caller.matches);
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

void SetChunkBorders(const ChunkBorderSettings& settings) {
    std::lock_guard lock(g_settingsMutex);
    g_chunks = settings;
}

void SetWorldShapes(std::vector<WorldShapes> shapes, int channel) {
    if (channel < 0 || channel >= kChannelCount) return;
    std::lock_guard lock(g_settingsMutex);
    g_shapes[channel] = std::move(shapes);
}

void SetWorldShapesMaterial(int index) {
    std::lock_guard lock(g_settingsMutex);
    g_shapesMaterial = index < 0 ? 0 : (index >= kFillMaterialCount ? kFillMaterialCount - 1 : index);
}

void OnRenderLevel(std::uintptr_t levelRenderer, std::uintptr_t screenContext) {
    // Ya no dibujamos aquí: fuera de una llamada del juego no se ve (ver
    // "Dibujar DENTRO de las llamadas de mallas"). Solo preparamos los
    // materiales la primera vez.
    (void)levelRenderer;
    (void)screenContext;
    if (g_ready.load(std::memory_order_relaxed)) {
        EnsureMaterials();
    }
}

}  // namespace luma::sdk::draw3d
