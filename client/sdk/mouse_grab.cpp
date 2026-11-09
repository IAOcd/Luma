// =============================================================================
//  mouse_grab.cpp - Implementación del control del mouse del juego
// =============================================================================
#include "sdk/mouse_grab.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/timer.h"
#include "render/notifications.h"
#include "render/overlay.h"
#include "sdk/scanner.h"

namespace luma::sdk::mouse {

namespace {

// --- Firmas (Minecraft Bedrock GDK 1.26.63) ---------------------------------------
// Inicio de cada función. Los ?? son direcciones relativas que cambian entre
// versiones (llamadas a funciones de Windows, variables globales, etc.).
//
// agarrar: push rsi/rdi/rbx; sub rsp,60h; mov rsi,rcx; (cookie de seguridad);
//          cmp byte [rcx+?],0; jne ...; xor ecx,ecx; call [ShowCursor]; ...
constexpr const char* kGrabPattern =
    "56 57 53 48 83 EC 60 48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 58 "
    "80 B9 ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 31 C9 FF 15 ?? ?? ?? ?? 85 C0 78 ?? 48 8B 3D";
// soltar: push rsi/rdi; sub rsp,28h; mov rsi,rcx; mov ecx,1; call [ShowCursor];
//         ...; xor ecx,ecx; call [ClipCursor]; call [ReleaseCapture];
//         mov byte [rsi+OFFSET],0   <- OFFSET = dónde guarda "mouse agarrado"
constexpr const char* kReleasePattern =
    "56 57 48 83 EC 28 48 89 CE B9 01 00 00 00 FF 15 ?? ?? ?? ?? 85 C0 79 ?? 48 8B 3D "
    "?? ?? ?? ?? 90 B9 01 00 00 00 FF D7 85 C0 78 ?? 31 C9 FF 15 ?? ?? ?? ?? FF 15 "
    "?? ?? ?? ?? C6 86 ?? ?? ?? ?? 00";
// Posición, dentro de la firma de "soltar", de los 4 bytes con OFFSET.
constexpr std::size_t kGrabbedOffsetPosition = 59;

// Las dos funciones reciben un solo parámetro: el objeto de la ventana del
// juego (en el registro rcx). Devolvemos lo que devuelvan, por si el que las
// llama lo usa.
using MouseFn = std::uintptr_t(__fastcall*)(void* window);
MouseFn g_originalGrab = nullptr;
MouseFn g_originalRelease = nullptr;

std::uint32_t g_grabbedOffset = 0;    // ventana + este desplazamiento = "mouse agarrado"
std::atomic<void*> g_window{nullptr}; // el objeto de la ventana (lo vemos cuando el juego llama)
std::atomic<bool> g_active{false};
bool g_grabOnClose = false;  // ¿hay que devolverle el mouse al juego al cerrar el menú?
bool g_warnedNoWindow = false;
bool g_releasedByUs = false;

// Lee "mouse agarrado" del objeto del juego. __try/__except: si por algún
// motivo la memoria no fuera válida, en vez de crashear devuelve false.
bool ReadGrabbed(void* window) {
    __try {
        return *(reinterpret_cast<volatile std::uint8_t*>(window) + g_grabbedOffset) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t __fastcall GrabDetour(void* window) {
    hooks::ActiveCallGuard guard;
    g_window.store(window);
    if (g_active.load() && render::overlay::IsMenuOpen()) {
        // El juego quiere el mouse, pero el menú de Luma está abierto:
        // se lo damos al cerrarlo.
        g_grabOnClose = true;
        return 0;
    }
    return g_originalGrab(window);
}

std::uintptr_t __fastcall ReleaseDetour(void* window) {
    hooks::ActiveCallGuard guard;
    g_window.store(window);
    if (g_active.load() && render::overlay::IsMenuOpen()) {
        // El juego mismo soltó el mouse (por ejemplo, la ventana perdió el
        // foco): al cerrar el menú NO hay que volver a agarrarlo.
        g_grabOnClose = false;
    }
    return g_originalRelease(window);
}

bool HookFunction(std::uintptr_t address, void* detour, void** original, const char* name) {
    void* target = reinterpret_cast<void*>(address);
    return hooks::Create(target, detour, original, name) && hooks::Enable(target, name);
}

}  // namespace

bool Init() {
    const std::int64_t start = perf::Now();
    const ScanResult grab = FindPattern(kGrabPattern);
    const ScanResult release = FindPattern(kReleasePattern);
    const double ms = perf::TicksToMs(perf::Now() - start);

    if (grab.matches != 1 || release.matches != 1) {
        log::Warn("SDK: firmas del mouse no encontradas (agarrar: {} coincidencias, soltar: {}). "
                  "Con el menú abierto, la cámara seguirá moviéndose. ¿El juego se actualizó?",
                  grab.matches, release.matches);
        notify::Push(notify::Kind::Warning, "Firma no encontrada: el menú no bloqueará la cámara");
        return false;
    }
    // Leer el desplazamiento de "mouse agarrado" de la propia instrucción
    // (así, si una versión nueva lo mueve, lo seguimos encontrando).
    g_grabbedOffset = *reinterpret_cast<const std::uint32_t*>(release.address + kGrabbedOffsetPosition);
    if (g_grabbedOffset < 0x10 || g_grabbedOffset > 0x10000) {
        log::Warn("SDK: desplazamiento de \"mouse agarrado\" sospechoso (0x{:X}); no se usa.", g_grabbedOffset);
        return false;
    }

    const bool ok =
        HookFunction(grab.address, reinterpret_cast<void*>(&GrabDetour),
                     reinterpret_cast<void**>(&g_originalGrab), "juego: agarrar mouse") &&
        HookFunction(release.address, reinterpret_cast<void*>(&ReleaseDetour),
                     reinterpret_cast<void**>(&g_originalRelease), "juego: soltar mouse");
    if (!ok) {
        return false;
    }
    g_active.store(true);
    log::Info("SDK: funciones del mouse encontradas en {:.0f} ms (agarrar: juego+0x{:X}, soltar: "
              "juego+0x{:X}, indicador en +0x{:X}).",
              ms, grab.address - GameBase(), release.address - GameBase(), g_grabbedOffset);
    return true;
}

bool Available() {
    return g_active.load();
}

bool ReleasedByLuma() {
    return g_releasedByUs;
}

void OnMenuOpened() {
    g_releasedByUs = false;
    if (!g_active.load()) {
        return;
    }
    void* window = g_window.load();
    if (window == nullptr) {
        // Todavía no vimos al juego agarrar o soltar el mouse, así que no
        // conocemos el objeto de su ventana.
        if (!g_warnedNoWindow) {
            g_warnedNoWindow = true;
            log::Info("SDK: todavía no se conoce la ventana del juego (pulsa Esc una vez en el juego).");
            notify::Push(notify::Kind::Info, "Para bloquear la cámara: cierra el menú, pulsa Esc dos veces");
        }
        return;
    }
    g_grabOnClose = ReadGrabbed(window);
    if (g_grabOnClose) {
        g_originalRelease(window);  // lo mismo que hace la pausa
        g_releasedByUs = true;
    }
}

void OnMenuClosed() {
    void* window = g_window.load();
    if (g_active.load() && g_grabOnClose && window != nullptr) {
        g_originalGrab(window);  // lo mismo que al volver a jugar
    }
    g_grabOnClose = false;
    g_releasedByUs = false;
}

void BeginUnload() {
    if (g_active.load() && render::overlay::IsMenuOpen()) {
        OnMenuClosed();
    }
    g_active.store(false);
}

}  // namespace luma::sdk::mouse
