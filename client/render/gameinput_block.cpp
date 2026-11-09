// =============================================================================
//  gameinput_block.cpp - Implementación del filtro de GameInput
// =============================================================================
#include "render/gameinput_block.h"

#include <windows.h>
#include <GameInput.h>  // Solo para los tipos; no enlazamos GameInput.lib.
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "core/hooks.h"
#include "core/logger.h"
#include "render/overlay.h"

using Microsoft::WRL::ComPtr;

namespace luma::render::gameinput {

namespace {

// Posición de cada método en la vtable de IGameInputReading (contando desde 0,
// en el orden en que GameInput.h los declara: QueryInterface, AddRef, Release,
// GetInputKind, GetSequenceNumber, GetTimestamp, GetDevice, GetRawReport,
// 6 métodos de controles, GetKeyCount, GetKeyState, GetMouseState, ...).
constexpr int kGetKeyCountIndex = 14;
constexpr int kGetKeyStateIndex = 15;
constexpr int kGetMouseStateIndex = 16;

using GameInputCreateFn = HRESULT(WINAPI*)(IGameInput**);
using GetKeyCountFn = std::uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*);
using GetKeyStateFn = std::uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, std::uint32_t,
                                                        GameInputKeyState*);
using GetMouseStateFn = bool(STDMETHODCALLTYPE*)(IGameInputReading*, GameInputMouseState*);

GetKeyCountFn g_originalGetKeyCount = nullptr;
GetKeyStateFn g_originalGetKeyState = nullptr;
GetMouseStateFn g_originalGetMouseState = nullptr;

std::atomic<bool> g_active{false};
bool g_loggedWaiting = false;

// Ocultar al juego el movimiento del mouse con el menú abierto.
//
// GameInput no entrega "cuánto se movió el mouse" sino la posición ACUMULADA
// (la suma de todo el movimiento desde que empezó). El juego calcula el
// movimiento restando dos lecturas seguidas. Si con el menú abierto solo
// "congeláramos" la posición, al cerrar el menú el juego vería de golpe todo
// lo acumulado y la cámara saltaría.
//
// Por eso llevamos un "desfase" (offset): con el menú abierto, todo lo que se
// mueve el mouse se suma al desfase, y SIEMPRE le restamos el desfase a lo que
// ve el juego. Así, para el juego, el mouse nunca se movió mientras el menú
// estuvo abierto.
//
// Limitación: al DESCARGAR Luma el desfase desaparece, y la cámara puede girar
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

bool STDMETHODCALLTYPE GetMouseStateDetour(IGameInputReading* reading, GameInputMouseState* state) {
    hooks::ActiveCallGuard guard;
    const bool ok = g_originalGetMouseState(reading, state);
    if (!ok || state == nullptr || !g_active.load(std::memory_order_relaxed)) {
        return ok;
    }
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
    return ok;
}

std::uint32_t STDMETHODCALLTYPE GetKeyCountDetour(IGameInputReading* reading) {
    hooks::ActiveCallGuard guard;
    if (BlockingNow()) {
        return 0;  // "No hay ninguna tecla apretada."
    }
    return g_originalGetKeyCount(reading);
}

std::uint32_t STDMETHODCALLTYPE GetKeyStateDetour(IGameInputReading* reading, std::uint32_t count,
                                                  GameInputKeyState* states) {
    hooks::ActiveCallGuard guard;
    if (BlockingNow()) {
        return 0;
    }
    return g_originalGetKeyState(reading, count, states);
}

void* VTableEntry(void* object, int index) {
    return (*static_cast<void***>(object))[index];
}

bool CreateAndEnable(void* target, void* detour, void** original, const char* name) {
    return hooks::Create(target, detour, original, name) && hooks::Enable(target, name);
}

}  // namespace

bool TryInstall() {
    // El juego carga la versión redistribuible de GameInput. Usamos la MISMA
    // DLL que el juego: así las lecturas son objetos de la misma clase y sus
    // métodos están en las mismas direcciones.
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

    // Nuestra propia instancia de GameInput, solo para conseguir una lectura
    // y leer su vtable. Se libera sola al salir (ComPtr).
    ComPtr<IGameInput> input;
    if (FAILED(create(&input))) {
        log::Warn("No se pudo crear una instancia de GameInput.");
        return true;
    }
    ComPtr<IGameInputReading> reading;
    if (FAILED(input->GetCurrentReading(GameInputKindMouse, nullptr, &reading)) &&
        FAILED(input->GetCurrentReading(GameInputKindKeyboard, nullptr, &reading))) {
        // Todavía no hay lecturas (GameInput solo guarda lecturas desde que se
        // crea la instancia). Se vuelve a intentar en un momento.
        if (!g_loggedWaiting) {
            g_loggedWaiting = true;
            log::Info("GameInput: esperando una lectura de mouse/teclado para instalar el filtro...");
        }
        return false;
    }

    bool ok = CreateAndEnable(VTableEntry(reading.Get(), kGetMouseStateIndex),
                              reinterpret_cast<void*>(&GetMouseStateDetour),
                              reinterpret_cast<void**>(&g_originalGetMouseState),
                              "IGameInputReading::GetMouseState");
    ok &= CreateAndEnable(VTableEntry(reading.Get(), kGetKeyCountIndex),
                          reinterpret_cast<void*>(&GetKeyCountDetour),
                          reinterpret_cast<void**>(&g_originalGetKeyCount),
                          "IGameInputReading::GetKeyCount");
    ok &= CreateAndEnable(VTableEntry(reading.Get(), kGetKeyStateIndex),
                          reinterpret_cast<void*>(&GetKeyStateDetour),
                          reinterpret_cast<void**>(&g_originalGetKeyState),
                          "IGameInputReading::GetKeyState");
    g_active.store(ok);
    if (ok) {
        log::Info("Filtro de GameInput activo: con el menú abierto, el juego no recibe mouse ni teclado.");
    }
    return true;
}

void BeginUnload() {
    g_active.store(false);
}

}  // namespace luma::render::gameinput
