// =============================================================================
//  hooks.cpp - MinHook wrapper implementation
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
        log::Error("MinHook couldn't start: {}", MH_StatusToString(status));
        return false;
    }
    g_initialized = true;
    return true;
}

bool Create(void* target, void* detour, void** original, const char* name) {
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        log::Error("Couldn't create hook {}: {}", name, MH_StatusToString(status));
        return false;
    }
    log::Info("Hook created: {} (0x{:X})", name, reinterpret_cast<std::uintptr_t>(target));
    return true;
}

bool EnableAll() {
    // MH_ALL_HOOKS = "every hook created". Enabling them all together is
    // better than one by one: MinHook pauses the game's threads only once.
    const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        log::Error("Couldn't enable the hooks: {}", MH_StatusToString(status));
        return false;
    }
    return true;
}

bool Enable(void* target, const char* name) {
    const MH_STATUS status = MH_EnableHook(target);
    if (status != MH_OK) {
        log::Error("Couldn't enable hook {}: {}", name, MH_StatusToString(status));
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
    // We wait in 10 ms steps. If it takes long, we note it in the log
    // (for example, if you're dragging the game window, Windows holds the
    // window thread inside our message handler).
    int waitedMs = 0;
    while (g_activeCalls.load(std::memory_order_acquire) > 0) {
        Sleep(10);
        waitedMs += 10;
        if (waitedMs == 2000) {
            log::Warn("Waiting for the game to leave Luma's code...");
        }
    }
    // Extra margin: a thread could have jumped into our detour right before
    // the hook was disabled and not yet added 1 to the counter (it's a matter of
    // nanoseconds). 100 ms covers that case with room to spare.
    Sleep(100);
}

void Shutdown() {
    if (!g_initialized) {
        return;
    }
    // Removes every hook and frees the trampolines' memory.
    MH_Uninitialize();
    g_initialized = false;
}

}  // namespace luma::hooks
