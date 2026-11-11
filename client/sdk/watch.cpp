// =============================================================================
//  watch.cpp - Puntos de vigilancia de memoria (ver watch.h)
// =============================================================================
#include "sdk/watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <format>
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
std::atomic<std::uint32_t> g_which[kSlots];  // bits: 1 = dirección 0, 2 = la 1, 4 = la 2, 8 = la 3
// Quién llamó (la primera vez de cada instrucción): direcciones de retorno que
// encontramos en la pila y que caen dentro del código del juego.
constexpr int kFrames = 8;
std::uintptr_t g_stack[kSlots][kFrames];
std::uintptr_t g_gameLo = 0;
std::uintptr_t g_gameHi = 0;

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    const DWORD64 dr6 = c->Dr6;
    if ((dr6 & 0xF) == 0) return EXCEPTION_CONTINUE_SEARCH;  // no es nuestro
    const std::uintptr_t rip = static_cast<std::uintptr_t>(c->Rip);
    const std::uint32_t which = static_cast<std::uint32_t>(dr6 & 0xF);
    for (int i = 0; i < kSlots; ++i) {
        std::uintptr_t expected = 0;
        const bool first = g_rip[i].load() != rip && g_rip[i].compare_exchange_strong(expected, rip);
        if (first || g_rip[i].load() == rip) {
            if (first) {
                // Mirar la pila (con cuidado: solo leer lo que se puede leer).
                const std::uintptr_t* sp = reinterpret_cast<const std::uintptr_t*>(c->Rsp);
                int found = 0;
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(sp, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT) {
                    const auto end = reinterpret_cast<const std::uintptr_t*>(static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize);
                    for (const std::uintptr_t* q = sp; q < end && q < sp + 512 && found < kFrames; ++q) {
                        if (*q >= g_gameLo && *q < g_gameHi) g_stack[i][found++] = *q;
                    }
                }
            }
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
bool SetDebugRegisters(DWORD threadId, const std::uintptr_t* a, bool enable) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (thread == nullptr) return false;
    bool ok = false;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &c)) {
            if (enable) {
                // DR0..DR3 = las direcciones; DR7: cada una encendida (bit 2*i), escritura
                // (RW = 01) y 8 bytes (LEN = 10).
                DWORD64 dr7 = 0;
                DWORD64* regs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
                for (int i = 0; i < 4; ++i) {
                    *regs[i] = a[i];
                    if (a[i] == 0) continue;
                    dr7 |= 1ull << (2 * i);
                    dr7 |= (0b01ull << (16 + 4 * i)) | (0b10ull << (18 + 4 * i));
                }
                c.Dr7 = dr7;
            } else {
                c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
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

// Lo mismo en TODOS los hilos del juego (menos el que llama).
int SetDebugRegistersAll(const std::uintptr_t* a, bool enable) {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (SetDebugRegisters(te.th32ThreadID, a, enable)) ++count;
    }
    CloseHandle(snap);
    return count;
}

}  // namespace

void Start(std::uint32_t threadId, std::uintptr_t address0, std::uintptr_t address1, int ms, const char* label,
           std::uintptr_t address2, std::uintptr_t address3) {
    if (g_used.exchange(true)) return;  // una sola vez por carga de Luma
    std::string name = label;
    // Otro hilo hace el trabajo (para pausar al de la interfaz hace falta ser otro).
    std::thread([threadId, address0, address1, address2, address3, ms, name] {
        for (int i = 0; i < kSlots; ++i) {
            g_rip[i].store(0);
            g_hits[i].store(0);
            g_which[i].store(0);
        }
        g_gameLo = GameBase();
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_gameLo);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_gameLo + dos->e_lfanew);
            g_gameHi = g_gameLo + nt->OptionalHeader.SizeOfImage;
        }
        for (auto& row : g_stack) for (auto& v : row) v = 0;
        g_handler = AddVectoredExceptionHandler(1, Handler);
        if (g_handler == nullptr) return;
        // Las direcciones deben estar alineadas a 8 bytes para vigilar 8.
        const std::uintptr_t a[4] = {address0 & ~std::uintptr_t{7}, address1 & ~std::uintptr_t{7}, address2 & ~std::uintptr_t{7},
                                     address3 & ~std::uintptr_t{7}};
        const std::uintptr_t none[4] = {0, 0, 0, 0};
        const bool all = threadId == 0;  // 0 = todos los hilos
        if (all ? SetDebugRegistersAll(a, true) == 0 : !SetDebugRegisters(threadId, a, true)) {
            log::Warn("Vigilancia: no se pudieron poner los registros de depuración.");
            RemoveVectoredExceptionHandler(g_handler);
            return;
        }
        g_active.store(true);
        log::Info("Vigilancia ({}): mirando escrituras en 0x{:X}, 0x{:X}, 0x{:X} y 0x{:X} durante {} ms...", name, a[0], a[1], a[2], a[3], ms);
        Sleep(static_cast<DWORD>(ms));
        if (all) {
            SetDebugRegistersAll(none, false);
        } else {
            SetDebugRegisters(threadId, none, false);
        }
        g_active.store(false);
        Sleep(50);  // por si quedaba una excepción en camino
        RemoveVectoredExceptionHandler(g_handler);
        const std::uintptr_t base = GameBase();
        int shown = 0;
        for (int i = 0; i < kSlots; ++i) {
            const std::uintptr_t rip = g_rip[i].load();
            if (rip == 0) continue;
            ++shown;
            std::string callers;
            for (int f = 0; f < kFrames && g_stack[i][f] != 0; ++f) {
                callers += std::format(" +0x{:X}", g_stack[i][f] - base);
            }
            log::Info("Vigilancia ({}): escritura desde juego+0x{:X} ({} veces, dirección {}). Pila:{}", name,
                      rip >= base ? rip - base : rip, g_hits[i].load(), g_which[i].load(), callers);
        }
        if (shown == 0) log::Info("Vigilancia ({}): nadie escribió.", name);
    }).detach();
}

}  // namespace luma::sdk::watch
