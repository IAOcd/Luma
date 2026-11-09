// =============================================================================
//  gameinput_block.cpp - Implementación del filtro de GameInput
// =============================================================================
//  Qué aprendimos probando (Fase 3):
//    - El teclado le llega al juego por GetAsyncKeyState (eso ya se bloquea
//      en input.cpp).
//    - La cámara y los clics siguen llegando juntos: el único canal que
//      entrega movimiento Y botones en un mismo paquete es GameInput
//      (GameInputMouseState). Raw input y GetCursorPos ya están descartados.
//    - Pero el GetMouseState que hookeamos (sacado de UNA lectura nuestra)
//      nunca recibió llamadas: las lecturas del juego deben ser de otra
//      clase, con su propia función.
//
//  Solución: también hookeamos los métodos de IGameInput con los que el juego
//  PIDE sus lecturas (GetCurrentReading / GetNextReading / GetPreviousReading).
//  Cada vez que el juego recibe una lectura, miramos de qué clase es (su
//  vtable) y, si es nueva, el hilo principal de Luma hookea SU GetMouseState,
//  GetKeyCount y GetKeyState. Así cubrimos todas las clases que use el juego,
//  sin adivinar.
// =============================================================================
#include "render/gameinput_block.h"

#include <windows.h>
#include <GameInput.h>  // Solo para los tipos; no enlazamos GameInput.lib.
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "core/hooks.h"
#include "core/logger.h"
#include "render/input_diag.h"
#include "render/overlay.h"

using Microsoft::WRL::ComPtr;

namespace luma::render::gameinput {

namespace {

// --- Posiciones en las vtables (orden de GameInput.h, contando desde 0) --------
// IGameInput: QueryInterface, AddRef, Release, GetCurrentTimestamp,
//             GetCurrentReading (4), GetNextReading (5), GetPreviousReading (6)...
constexpr int kGetCurrentReadingIndex = 4;
constexpr int kGetNextReadingIndex = 5;
constexpr int kGetPreviousReadingIndex = 6;
// IGameInputReading: ..., GetKeyCount (14), GetKeyState (15), GetMouseState (16)
constexpr int kGetKeyCountIndex = 14;
constexpr int kGetKeyStateIndex = 15;
constexpr int kGetMouseStateIndex = 16;

using GameInputCreateFn = HRESULT(WINAPI*)(IGameInput**);
using GetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, GameInputKind,
                                                        IGameInputDevice*, IGameInputReading**);
using GetRelativeReadingFn = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, IGameInputReading*,
                                                         GameInputKind, IGameInputDevice*,
                                                         IGameInputReading**);
using GetKeyCountFn = std::uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*);
using GetKeyStateFn = std::uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, std::uint32_t,
                                                        GameInputKeyState*);
using GetMouseStateFn = bool(STDMETHODCALLTYPE*)(IGameInputReading*, GameInputMouseState*);

GetCurrentReadingFn g_originalGetCurrentReading = nullptr;
GetRelativeReadingFn g_originalGetNextReading = nullptr;
GetRelativeReadingFn g_originalGetPreviousReading = nullptr;

std::atomic<bool> g_active{false};
bool g_instanceHooked = false;
bool g_loggedWaiting = false;

// --- Filtro del mouse ---------------------------------------------------------------
// GameInput no entrega "cuánto se movió el mouse" sino la posición ACUMULADA.
// El juego calcula el movimiento restando dos lecturas. Para que no vea
// movimiento con el menú abierto (ni un salto al cerrarlo), llevamos un
// "desfase": todo lo que se mueve el mouse con el menú abierto se suma al
// desfase, y SIEMPRE se lo restamos a lo que ve el juego.
//
// Limitación: al DESCARGAR Luma el desfase desaparece y la cámara puede girar
// una vez de golpe (tanto como moviste el mouse con el menú abierto).
struct MouseFilter {
    std::mutex mutex;
    bool hasLast = false;
    std::int64_t lastX = 0, lastY = 0, lastWheelX = 0, lastWheelY = 0;
    std::int64_t offsetX = 0, offsetY = 0, offsetWheelX = 0, offsetWheelY = 0;
};
MouseFilter g_mouse;

bool BlockingNow() {
    return g_active.load(std::memory_order_relaxed) && overlay::IsMenuOpen();
}

void FilterMouseState(GameInputMouseState* state) {
    const bool block = overlay::IsMenuOpen();
    std::lock_guard lock(g_mouse.mutex);
    if (g_mouse.hasLast && block) {
        g_mouse.offsetX += state->positionX - g_mouse.lastX;
        g_mouse.offsetY += state->positionY - g_mouse.lastY;
        g_mouse.offsetWheelX += state->wheelX - g_mouse.lastWheelX;
        g_mouse.offsetWheelY += state->wheelY - g_mouse.lastWheelY;
    }
    g_mouse.hasLast = true;
    g_mouse.lastX = state->positionX;
    g_mouse.lastY = state->positionY;
    g_mouse.lastWheelX = state->wheelX;
    g_mouse.lastWheelY = state->wheelY;

    state->positionX -= g_mouse.offsetX;
    state->positionY -= g_mouse.offsetY;
    state->wheelX -= g_mouse.offsetWheelX;
    state->wheelY -= g_mouse.offsetWheelY;
    if (block) {
        state->buttons = GameInputMouseNone;  // sin clics
    }
}

// --- Hooks por clase de lectura -----------------------------------------------
// Cada clase de lectura tiene sus propias funciones, y cada hook necesita su
// propio "trampolín" (puntero a la original). Como C++ no deja crear funciones
// nuevas en tiempo de ejecución, preparamos de antemano kMaxClasses copias de
// cada detour con un "template": MouseDetour<0>, MouseDetour<1>, ...
constexpr int kMaxClasses = 6;

struct ReadingClassHooks {
    void* mouseTarget = nullptr;     // dirección de su GetMouseState
    void* keyCountTarget = nullptr;
    void* keyStateTarget = nullptr;
    GetMouseStateFn originalMouse = nullptr;
    GetKeyCountFn originalKeyCount = nullptr;
    GetKeyStateFn originalKeyState = nullptr;
};
std::array<ReadingClassHooks, kMaxClasses> g_classes{};
// Solo lo cambia el hilo principal, pero lo leen los hilos del juego: atómico.
std::atomic<int> g_classCount{0};

// Vtables vistas por los detours del juego y todavía no hookeadas. Los detours
// solo las anotan; el hilo principal hace el trabajo pesado (crear hooks).
std::array<std::atomic<void**>, kMaxClasses> g_pendingVtables{};

template <int N>
bool STDMETHODCALLTYPE MouseDetour(IGameInputReading* reading, GameInputMouseState* state) {
    hooks::ActiveCallGuard guard;
    diag::Count(diag::g_counters.gameInputMouseState);
    const bool ok = g_classes[N].originalMouse(reading, state);
    if (ok && state != nullptr && g_active.load(std::memory_order_relaxed)) {
        FilterMouseState(state);
    }
    return ok;
}

template <int N>
std::uint32_t STDMETHODCALLTYPE KeyCountDetour(IGameInputReading* reading) {
    hooks::ActiveCallGuard guard;
    if (BlockingNow()) {
        return 0;  // "No hay ninguna tecla apretada."
    }
    return g_classes[N].originalKeyCount(reading);
}

template <int N>
std::uint32_t STDMETHODCALLTYPE KeyStateDetour(IGameInputReading* reading, std::uint32_t count,
                                               GameInputKeyState* states) {
    hooks::ActiveCallGuard guard;
    diag::Count(diag::g_counters.gameInputKeyState);
    if (BlockingNow()) {
        return 0;
    }
    return g_classes[N].originalKeyState(reading, count, states);
}

// Tablas con las direcciones de cada copia de los detours.
template <int... N>
constexpr std::array<void*, sizeof...(N)> MakeMouseDetours(std::integer_sequence<int, N...>) {
    return {reinterpret_cast<void*>(&MouseDetour<N>)...};
}
template <int... N>
constexpr std::array<void*, sizeof...(N)> MakeKeyCountDetours(std::integer_sequence<int, N...>) {
    return {reinterpret_cast<void*>(&KeyCountDetour<N>)...};
}
template <int... N>
constexpr std::array<void*, sizeof...(N)> MakeKeyStateDetours(std::integer_sequence<int, N...>) {
    return {reinterpret_cast<void*>(&KeyStateDetour<N>)...};
}
const auto kMouseDetours = MakeMouseDetours(std::make_integer_sequence<int, kMaxClasses>{});
const auto kKeyCountDetours = MakeKeyCountDetours(std::make_integer_sequence<int, kMaxClasses>{});
const auto kKeyStateDetours = MakeKeyStateDetours(std::make_integer_sequence<int, kMaxClasses>{});

bool IsKnownMouseTarget(void* target) {
    const int count = g_classCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (g_classes[i].mouseTarget == target) {
            return true;
        }
    }
    return false;
}

// Hookea las funciones de una clase de lectura (hilo principal).
void HookReadingClass(void** vtable, const char* origin) {
    void* mouse = vtable[kGetMouseStateIndex];
    if (IsKnownMouseTarget(mouse)) {
        return;  // Esa función ya está hookeada (varias clases pueden compartirla).
    }
    const int n = g_classCount.load();
    if (n >= kMaxClasses) {
        log::Warn("GameInput: demasiadas clases de lectura distintas; se ignora una ({}).", origin);
        return;
    }
    ReadingClassHooks& c = g_classes[n];
    c.mouseTarget = mouse;
    c.keyCountTarget = vtable[kGetKeyCountIndex];
    c.keyStateTarget = vtable[kGetKeyStateIndex];

    bool ok = hooks::Create(c.mouseTarget, kMouseDetours[n], reinterpret_cast<void**>(&c.originalMouse),
                            "IGameInputReading::GetMouseState") &&
              hooks::Enable(c.mouseTarget, "IGameInputReading::GetMouseState");
    // GetKeyCount/GetKeyState pueden ser funciones compartidas con otra clase
    // ya hookeada: si MinHook dice que ya existe, no es un error.
    if (hooks::Create(c.keyCountTarget, kKeyCountDetours[n],
                      reinterpret_cast<void**>(&c.originalKeyCount), "IGameInputReading::GetKeyCount")) {
        hooks::Enable(c.keyCountTarget, "IGameInputReading::GetKeyCount");
    }
    if (hooks::Create(c.keyStateTarget, kKeyStateDetours[n],
                      reinterpret_cast<void**>(&c.originalKeyState), "IGameInputReading::GetKeyState")) {
        hooks::Enable(c.keyStateTarget, "IGameInputReading::GetKeyState");
    }
    if (ok) {
        g_classCount.store(n + 1, std::memory_order_release);
        log::Info("GameInput: clase de lectura #{} hookeada (vtable 0x{:X}, origen: {}).", n,
                  reinterpret_cast<std::uintptr_t>(vtable), origin);
    }
}

// Desde los detours del juego: anotar la clase de una lectura si es nueva.
void NoteReading(IGameInputReading* reading) {
    if (reading == nullptr) {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(reading);
    if (IsKnownMouseTarget(vtable[kGetMouseStateIndex])) {
        return;  // caso normal: ya conocida (solo una comparación)
    }
    for (auto& slot : g_pendingVtables) {
        void** expected = nullptr;
        if (slot.load(std::memory_order_relaxed) == vtable ||
            slot.compare_exchange_strong(expected, vtable)) {
            return;
        }
    }
}

// --- Hooks de IGameInput (cómo el juego pide sus lecturas) -----------------------

HRESULT STDMETHODCALLTYPE GetCurrentReadingDetour(IGameInput* input, GameInputKind kind,
                                                  IGameInputDevice* device,
                                                  IGameInputReading** reading) {
    hooks::ActiveCallGuard guard;
    diag::Count(diag::g_counters.gameInputGetReading);
    const HRESULT hr = g_originalGetCurrentReading(input, kind, device, reading);
    if (SUCCEEDED(hr) && reading != nullptr) {
        NoteReading(*reading);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetNextReadingDetour(IGameInput* input, IGameInputReading* reference,
                                               GameInputKind kind, IGameInputDevice* device,
                                               IGameInputReading** reading) {
    hooks::ActiveCallGuard guard;
    diag::Count(diag::g_counters.gameInputGetReading);
    const HRESULT hr = g_originalGetNextReading(input, reference, kind, device, reading);
    if (SUCCEEDED(hr) && reading != nullptr) {
        NoteReading(*reading);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetPreviousReadingDetour(IGameInput* input, IGameInputReading* reference,
                                                   GameInputKind kind, IGameInputDevice* device,
                                                   IGameInputReading** reading) {
    hooks::ActiveCallGuard guard;
    diag::Count(diag::g_counters.gameInputGetReading);
    const HRESULT hr = g_originalGetPreviousReading(input, reference, kind, device, reading);
    if (SUCCEEDED(hr) && reading != nullptr) {
        NoteReading(*reading);
    }
    return hr;
}

void* VTableEntry(void* object, int index) {
    return (*static_cast<void***>(object))[index];
}

bool HookInstanceMethods(IGameInput* input) {
    struct Spec {
        int index;
        void* detour;
        void** original;
        const char* name;
    };
    const Spec specs[] = {
        {kGetCurrentReadingIndex, reinterpret_cast<void*>(&GetCurrentReadingDetour),
         reinterpret_cast<void**>(&g_originalGetCurrentReading), "IGameInput::GetCurrentReading"},
        {kGetNextReadingIndex, reinterpret_cast<void*>(&GetNextReadingDetour),
         reinterpret_cast<void**>(&g_originalGetNextReading), "IGameInput::GetNextReading"},
        {kGetPreviousReadingIndex, reinterpret_cast<void*>(&GetPreviousReadingDetour),
         reinterpret_cast<void**>(&g_originalGetPreviousReading), "IGameInput::GetPreviousReading"},
    };
    bool ok = true;
    for (const Spec& s : specs) {
        void* target = VTableEntry(input, s.index);
        ok &= hooks::Create(target, s.detour, s.original, s.name) && hooks::Enable(target, s.name);
    }
    return ok;
}

}  // namespace

bool TryInstall() {
    // Clases nuevas que vio el juego desde la última vuelta: hookearlas ahora.
    for (auto& slot : g_pendingVtables) {
        if (void** vtable = slot.exchange(nullptr)) {
            HookReadingClass(vtable, "lectura del juego");
        }
    }
    if (g_instanceHooked) {
        return false;  // seguimos atentos a clases nuevas mientras Luma esté cargado
    }

    // El juego carga la versión redistribuible de GameInput. Usamos la MISMA
    // DLL que el juego.
    HMODULE module = GetModuleHandleW(L"GameInputRedist.dll");
    if (module == nullptr) {
        module = GetModuleHandleW(L"GameInput.dll");
    }
    if (module == nullptr) {
        log::Info("El juego no tiene GameInput cargado; no hace falta filtrarlo.");
        return true;
    }
    auto create = reinterpret_cast<GameInputCreateFn>(GetProcAddress(module, "GameInputCreate"));
    if (create == nullptr) {
        log::Warn("GameInput cargado pero sin GameInputCreate; no se puede filtrar el mouse.");
        return true;
    }

    // Nuestra propia instancia: solo para leer las vtables. Se libera sola
    // al salir (ComPtr).
    ComPtr<IGameInput> input;
    if (FAILED(create(&input))) {
        log::Warn("No se pudo crear una instancia de GameInput.");
        return true;
    }

    // 1. Los métodos con los que el juego pide lecturas (para descubrir las
    //    clases que usa de verdad).
    if (!HookInstanceMethods(input.Get())) {
        log::Warn("GameInput: no se pudieron hookear los métodos de IGameInput.");
    }
    g_instanceHooked = true;
    g_active.store(true);

    // 2. Las clases de lectura que podamos conseguir nosotros mismos, por si
    //    coinciden con las del juego.
    const struct {
        GameInputKind kind;
        const char* name;
    } kinds[] = {{GameInputKindMouse, "mouse"}, {GameInputKindKeyboard, "teclado"},
                 {GameInputKindGamepad, "mando"}};
    for (const auto& k : kinds) {
        ComPtr<IGameInputReading> reading;
        if (SUCCEEDED(input->GetCurrentReading(k.kind, nullptr, &reading))) {
            HookReadingClass(*reinterpret_cast<void***>(reading.Get()), k.name);
        } else {
            log::Info("GameInput: todavía no hay lectura propia de {}.", k.name);
        }
    }
    log::Info("Filtro de GameInput activo. Se hookearán también las clases de lectura que use el juego.");
    return false;  // seguir llamando: hookea las clases nuevas que aparezcan
}

void BeginUnload() {
    g_active.store(false);
}

}  // namespace luma::render::gameinput
