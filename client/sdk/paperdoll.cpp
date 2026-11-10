// =============================================================================
//  paperdoll.cpp - Mover y agrandar el muñeco del HUD del juego (ver paperdoll.h)
// =============================================================================
#include "sdk/paperdoll.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "core/hooks.h"
#include "core/logger.h"
#include "sdk/memory.h"
#include "sdk/scanner.h"

namespace luma::sdk::paperdoll {

namespace {

// Una llamada a ScreenView::setupAndRender (E8 = "call"; la función es el destino).
constexpr const char* kSetupAndRenderCall =
    "E8 ?? ?? ?? ?? 90 48 8B 4B ?? 48 85 C9 74 ?? 48 8B 01 48 8B 40 ?? 48 89 FA FF 15 ?? ?? ?? ?? 90 48 8D 4D";

// --- Estructuras de la interfaz del juego (1.26.6x) -----------------------------------
constexpr std::uintptr_t kViewVisualTree = 0x50;   // ScreenView -> VisualTree
constexpr std::uintptr_t kTreeRoot = 0x08;         // VisualTree -> UIControl raíz
constexpr std::uintptr_t kControlPosition = 0x10;  // Vec2 (posición en la pantalla)
constexpr std::uintptr_t kControlName = 0x20;      // std::string
constexpr std::uintptr_t kControlRelative = 0x40;  // Vec2 (posición relativa al padre)
constexpr std::uintptr_t kControlBounds = 0x48;    // Vec2 (tamaño)
constexpr std::uintptr_t kControlParent = 0x88;    // shared_ptr<UIControl>
constexpr std::uintptr_t kControlChildren = 0x98;  // vector<shared_ptr<UIControl>>
constexpr std::uintptr_t kControlComponents = 0xB8;  // vector<UIComponent*>
constexpr std::size_t kRendererComponent = 4;      // el componente que tiene el renderer del muñeco
constexpr std::uintptr_t kComponentRenderer = 0x20;
constexpr std::uintptr_t kRendererTimeToClose = 0x10;  // float

// El tamaño "normal" del muñeco en unidades de la interfaz del juego.
constexpr float kDollSize = 15.0f;

using SetupAndRenderFn = void(__fastcall*)(void* view, void* ctx);
SetupAndRenderFn g_original = nullptr;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_unloading{false};

std::mutex g_mutex;
Settings g_settings;

std::atomic<float> g_rootW{0.0f};
std::atomic<float> g_rootH{0.0f};
std::atomic<bool> g_found{false};

// Estado del hilo de la interfaz.
std::uintptr_t g_doll = 0;        // el control "hud_player" (cache)
int g_framesSinceSearch = 1000;
bool g_modified = false;          // ¿le cambiamos posición/tamaño?
std::uintptr_t g_savedFor = 0;    // de qué control son los valores guardados
float g_savedPos[2]{};
float g_savedRel[2]{};
float g_savedBounds[2]{};

bool NameIs(std::uintptr_t control, const char* name) {
    char text[32];
    return SafeReadString(control + kControlName, text, sizeof(text)) && std::strcmp(text, name) == 0;
}

// Busca un control por nombre entre los descendientes (con límites: nunca
// recorre árboles raros sin fin).
std::uintptr_t FindDescendant(std::uintptr_t control, const char* name, int depth, int& budget) {
    if (depth > 16 || --budget < 0 || !LooksLikePointer(control)) {
        return 0;
    }
    if (NameIs(control, name)) {
        return control;
    }
    std::uintptr_t children[2]{};
    if (!SafeRead(control + kControlChildren, children) || children[1] < children[0] ||
        (children[1] - children[0]) / 16 > 512) {
        return 0;
    }
    for (std::uintptr_t it = children[0]; it < children[1]; it += 16) {  // shared_ptr = 2 punteros
        std::uintptr_t child = 0;
        if (SafeRead(it, child)) {
            if (const std::uintptr_t found = FindDescendant(child, name, depth + 1, budget)) {
                return found;
            }
        }
    }
    return 0;
}

void Restore() {
    if (g_modified && g_savedFor != 0 && NameIs(g_savedFor, "hud_player")) {
        SafeWrite(g_savedFor + kControlPosition, g_savedPos, sizeof(g_savedPos));
        SafeWrite(g_savedFor + kControlRelative, g_savedRel, sizeof(g_savedRel));
        SafeWrite(g_savedFor + kControlBounds, g_savedBounds, sizeof(g_savedBounds));
    }
    g_modified = false;
}

void Apply(std::uintptr_t root) {
    Settings s;
    {
        std::lock_guard lock(g_mutex);
        s = g_settings;
    }
    float rootSize[2]{};
    if (SafeRead(root + kControlBounds, rootSize) && rootSize[0] > 0.0f) {
        g_rootW.store(rootSize[0], std::memory_order_relaxed);
        g_rootH.store(rootSize[1], std::memory_order_relaxed);
    }
    if (!s.enabled) {
        Restore();
        return;
    }
    // Buscar el muñeco (cada tanto, o si el que teníamos ya no se llama así).
    if (g_doll == 0 || ++g_framesSinceSearch > 120 || !NameIs(g_doll, "hud_player")) {
        int budget = 3000;
        g_doll = FindDescendant(root, "hud_player", 0, budget);
        g_framesSinceSearch = 0;
        g_found.store(g_doll != 0, std::memory_order_relaxed);
    }
    if (g_doll == 0) {
        return;
    }
    // Guardar cómo estaba (para devolverlo al desactivar).
    if (g_savedFor != g_doll) {
        if (!SafeRead(g_doll + kControlPosition, g_savedPos) || !SafeRead(g_doll + kControlRelative, g_savedRel) ||
            !SafeRead(g_doll + kControlBounds, g_savedBounds)) {
            return;
        }
        g_savedFor = g_doll;
    }
    // s.x y s.y ya vienen en unidades de la interfaz del juego (el módulo las
    // convierte desde píxeles con RootWidth()).
    const float pos[2] = {s.x, s.y};
    const float bounds[2] = {kDollSize * s.scale, kDollSize * s.scale};
    // Posición relativa al padre = posición deseada - posición del padre.
    float rel[2] = {pos[0], pos[1]};
    std::uintptr_t parent = 0;
    float parentPos[2]{};
    if (SafeRead(g_doll + kControlParent, parent) && LooksLikePointer(parent) &&
        SafeRead(parent + kControlPosition, parentPos) && std::isfinite(parentPos[0]) && std::isfinite(parentPos[1])) {
        rel[0] -= parentPos[0];
        rel[1] -= parentPos[1];
    }
    SafeWrite(g_doll + kControlPosition, pos, sizeof(pos));
    SafeWrite(g_doll + kControlRelative, rel, sizeof(rel));
    SafeWrite(g_doll + kControlBounds, bounds, sizeof(bounds));
    g_modified = true;

    static bool logged = false;
    if (!logged) {
        // Diagnóstico (una vez): los componentes del muñeco y su temporizador.
        logged = true;
        std::uintptr_t comps[2]{};
        SafeRead(g_doll + kControlComponents, comps);
        const std::size_t n = comps[1] > comps[0] ? (comps[1] - comps[0]) / 8 : 0;
        log::Info("SDK: muñeco encontrado (0x{:X}), {} componentes, tamaño original {:.1f} x {:.1f}.", g_doll, n,
                  g_savedBounds[0], g_savedBounds[1]);
        for (std::size_t i = 0; i < n && i < 10; ++i) {
            std::uintptr_t comp = 0;
            std::uintptr_t rend = 0;
            float f[2]{};
            SafeRead(comps[0] + i * 8, comp);
            SafeRead(comp + kComponentRenderer, rend);
            SafeRead(rend + kRendererTimeToClose, f);
            log::Info("SDK:   componente {}: 0x{:X}, +0x20 -> 0x{:X}, +0x10 = {:.3f} {:.3f}", i, comp, rend, f[0], f[1]);
        }
    }
    if (s.alwaysShow) {
        // El temporizador de "cuánto falta para esconderlo": siempre lleno.
        std::uintptr_t comps[2]{};
        std::uintptr_t comp = 0;
        std::uintptr_t renderer = 0;
        float timeToClose = 0.0f;
        if (SafeRead(g_doll + kControlComponents, comps) && comps[1] > comps[0] &&
            (comps[1] - comps[0]) / 8 > kRendererComponent &&
            SafeRead(comps[0] + kRendererComponent * 8, comp) && LooksLikePointer(comp) &&
            SafeRead(comp + kComponentRenderer, renderer) && LooksLikePointer(renderer) &&
            SafeRead(renderer + kRendererTimeToClose, timeToClose) && std::isfinite(timeToClose) &&
            timeToClose >= -10.0f && timeToClose <= 10.0f) {  // escondido = negativo
            const float full = 1.0f;
            SafeWrite(renderer + kRendererTimeToClose, &full, sizeof(full));
        }
    }
}

void __fastcall SetupAndRenderDetour(void* view, void* ctx) {
    hooks::ActiveCallGuard guard;
    g_original(view, ctx);
    if (g_unloading.load(std::memory_order_relaxed)) {
        return;
    }
    // ¿Es la pantalla del HUD?
    std::uintptr_t tree = 0;
    std::uintptr_t root = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(view) + kViewVisualTree, tree) || !LooksLikePointer(tree) ||
        !SafeRead(tree + kTreeRoot, root) || !LooksLikePointer(root) || !NameIs(root, "hud_screen")) {
        return;
    }
    Apply(root);
}

}  // namespace

bool Init() {
    const ScanResult call = FindPattern(kSetupAndRenderCall);
    if (call.matches != 1) {
        log::Warn("SDK: no se encontró ScreenView::setupAndRender ({} coincidencias): el muñeco movible no estará "
                  "disponible.",
                  call.matches);
        return false;
    }
    void* target = reinterpret_cast<void*>(ResolveRipRelative(call.address + 1, call.address + 5));
    if (!hooks::Create(target, reinterpret_cast<void*>(&SetupAndRenderDetour), reinterpret_cast<void**>(&g_original),
                       "juego: dibujar la interfaz (ScreenView::setupAndRender)") ||
        !hooks::Enable(target, "juego: dibujar la interfaz (ScreenView::setupAndRender)")) {
        return false;
    }
    log::Info("SDK: interfaz del juego enganchada (juego+0x{:X}).", reinterpret_cast<std::uintptr_t>(target) - GameBase());
    g_ready.store(true);
    return true;
}

bool Available() {
    return g_ready.load(std::memory_order_relaxed);
}

void Set(const Settings& s) {
    std::lock_guard lock(g_mutex);
    g_settings = s;
}

float RootWidth() {
    return g_rootW.load(std::memory_order_relaxed);
}

bool Found() {
    return g_found.load(std::memory_order_relaxed);
}

void BeginUnload() {
    // Dejamos de tocar la interfaz y devolvemos el muñeco a su lugar (con
    // escrituras seguras: si el control ya no existe, no pasa nada).
    g_unloading.store(true);
    Restore();
}

}  // namespace luma::sdk::paperdoll
