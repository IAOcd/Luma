// =============================================================================
//  hooks.cpp - Implementación del envoltorio de MinHook
// =============================================================================
#include "core/hooks.h"

#include <windows.h>

#include <cstdint>

#include <MinHook.h>

#include "core/logger.h"

namespace luma::hooks {

std::atomic<int> g_activeCalls{0};

namespace {
bool g_initialized = false;
}  // namespace

bool Init() {
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        log::Error("MinHook no pudo iniciar: {}", MH_StatusToString(status));
        return false;
    }
    g_initialized = true;
    return true;
}

bool Create(void* target, void* detour, void** original, const char* name) {
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        log::Error("No se pudo crear el hook {}: {}", name, MH_StatusToString(status));
        return false;
    }
    log::Info("Hook creado: {} (0x{:X})", name, reinterpret_cast<std::uintptr_t>(target));
    return true;
}

bool EnableAll() {
    // MH_ALL_HOOKS = "todos los hooks creados". Activarlos todos juntos es
    // mejor que uno por uno: MinHook pausa los hilos del juego una sola vez.
    const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        log::Error("No se pudieron activar los hooks: {}", MH_StatusToString(status));
        return false;
    }
    return true;
}

bool Enable(void* target, const char* name) {
    const MH_STATUS status = MH_EnableHook(target);
    if (status != MH_OK) {
        log::Error("No se pudo activar el hook {}: {}", name, MH_StatusToString(status));
        return false;
    }
    return true;
}

void DisableAll() {
    if (g_initialized) {
        MH_DisableHook(MH_ALL_HOOKS);
    }
}

void WaitForActiveCalls() {
    // Esperamos en pasos de 10 ms. Si tarda mucho, lo anotamos en el log
    // (por ejemplo, si estás arrastrando la ventana del juego, Windows retiene
    // el hilo de la ventana dentro de nuestro procesador de mensajes).
    int waitedMs = 0;
    while (g_activeCalls.load(std::memory_order_acquire) > 0) {
        Sleep(10);
        waitedMs += 10;
        if (waitedMs == 2000) {
            log::Warn("Esperando a que el juego salga del código de Luma...");
        }
    }
    // Margen extra: un hilo podría haber saltado a nuestro detour justo antes
    // de desactivar el hook y todavía no haber sumado 1 al contador (son
    // nanosegundos). 100 ms cubren ese caso de sobra.
    Sleep(100);
}

void Shutdown() {
    if (!g_initialized) {
        return;
    }
    // Quita todos los hooks y libera la memoria de los trampolines.
    MH_Uninitialize();
    g_initialized = false;
}

}  // namespace luma::hooks
