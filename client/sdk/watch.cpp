// =============================================================================
//  watch.cpp - Puntos de vigilancia de memoria (ver watch.h)
// =============================================================================
#include "sdk/watch.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "core/logger.h"
#include "sdk/scanner.h"

namespace luma::sdk::watch {

namespace {

constexpr int kSlots = 32;
std::atomic<bool> g_used{false};
std::atomic<bool> g_active{false};
PVOID g_handler = nullptr;
// Lo que anota el manejador (sin pedir memoria: corre en medio del juego).
std::atomic<std::uintptr_t> g_rip[kSlots];
std::atomic<std::uint32_t> g_hits[kSlots];
std::atomic<std::uint32_t> g_which[kSlots];  // 1 = dirección 0, 2 = dirección 1

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    const DWORD64 dr6 = c->Dr6;
    if ((dr6 & 0x3) == 0) return EXCEPTION_CONTINUE_SEARCH;  // no es nuestro
    const std::uintptr_t rip = static_cast<std::uintptr_t>(c->Rip);
    const std::uint32_t which = static_cast<std::uint32_t>(dr6 & 0x3);
    for (int i = 0; i < kSlots; ++i) {
        std::uintptr_t expected = 0;
        if (g_rip[i].load() == rip || g_rip[i].compare_exchange_strong(expected, rip)) {
            g_hits[i].fetch_add(1);
            g_which[i].fetch_or(which);
            break;
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Pone o saca los registros de depuración de OTRO hilo (hay que pausarlo:
// un hilo no puede cambiarse los suyos de forma confiable).
bool SetDebugRegisters(DWORD threadId, std::uintptr_t a0, std::uintptr_t a1, bool enable) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (thread == nullptr) return false;
    bool ok = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &c)) {
            if (enable) {
                c.Dr0 = a0;
                c.Dr1 = a1;
                // DR7: L0 (bit 0) y L1 (bit 2) encendidos; RW = 01 (escritura) y LEN = 10 (8 bytes).
                DWORD64 dr7 = 0;
                dr7 |= 1ull << 0;
                dr7 |= (0b01ull << 16) | (0b10ull << 18);
                if (a1 != 0) {
                    dr7 |= 1ull << 2;
                    dr7 |= (0b01ull << 20) | (0b10ull << 22);
                }
                c.Dr7 = dr7;
            } else {
                c.Dr0 = c.Dr1 = 0;
                c.Dr7 = 0;
            }
            c.Dr6 = 0;
            ok = SetThreadContext(thread, &c) != 0;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return ok;
}

}  // namespace

void Start(std::uint32_t threadId, std::uintptr_t address0, std::uintptr_t address1, int ms, const char* label) {
    if (g_used.exchange(true)) return;  // una sola vez por carga de Luma
    std::string name = label;
    // Otro hilo hace el trabajo (para pausar al de la interfaz hace falta ser otro).
    std::thread([threadId, address0, address1, ms, name] {
        for (int i = 0; i < kSlots; ++i) {
            g_rip[i].store(0);
            g_hits[i].store(0);
            g_which[i].store(0);
        }
        g_handler = AddVectoredExceptionHandler(1, Handler);
        if (g_handler == nullptr) return;
        // Las direcciones deben estar alineadas a 8 bytes para vigilar 8.
        const std::uintptr_t a0 = address0 & ~std::uintptr_t{7};
        const std::uintptr_t a1 = address1 & ~std::uintptr_t{7};
        if (!SetDebugRegisters(threadId, a0, a1, true)) {
            log::Warn("Vigilancia: no se pudieron poner los registros de depuración.");
            RemoveVectoredExceptionHandler(g_handler);
            return;
        }
        g_active.store(true);
        log::Info("Vigilancia ({}): mirando escrituras en 0x{:X} y 0x{:X} durante {} ms...", name, a0, a1, ms);
        Sleep(static_cast<DWORD>(ms));
        SetDebugRegisters(threadId, 0, 0, false);
        g_active.store(false);
        Sleep(50);  // por si quedaba una excepción en camino
        RemoveVectoredExceptionHandler(g_handler);
        const std::uintptr_t base = GameBase();
        int shown = 0;
        for (int i = 0; i < kSlots; ++i) {
            const std::uintptr_t rip = g_rip[i].load();
            if (rip == 0) continue;
            ++shown;
            log::Info("Vigilancia ({}): escritura desde juego+0x{:X} ({} veces, dirección {}).", name,
                      rip >= base ? rip - base : rip, g_hits[i].load(), g_which[i].load());
        }
        if (shown == 0) log::Info("Vigilancia ({}): nadie escribió.", name);
    }).detach();
}

}  // namespace luma::sdk::watch
