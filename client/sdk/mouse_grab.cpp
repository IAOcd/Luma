// =============================================================================
//  mouse_grab.cpp - Implementación del control del mouse del juego
// =============================================================================
//  Hay DOS niveles de "agarrar / soltar el mouse" en el juego, y los dos
//  importan (lo descubrimos probando: el primero solo frenaba la cámara):
//
//    1. CLIENTE (en Bedrock se suele llamar ClientInstance): guarda un
//       indicador "mouse agarrado" (en +0x1F8 en el juego 1.26.63) que el
//       resto del juego consulta para decidir si un clic es un ataque. Sus
//       funciones agarrar/soltar cambian ese indicador y después llaman a las
//       de la ventana. Es lo que usan otros clients (Flarial, Onix) para su
//       menú. -> Es el camino PRINCIPAL.
//
//    2. VENTANA (la capa de plataforma): esconde/muestra el cursor, lo
//       encierra/libera (ClipCursor) y guarda su propio indicador (+0x398).
//       -> Es el RESPALDO si las funciones del cliente no se encuentran.
//
//  Cómo encontramos cada cosa está explicado en mouse_grab.h y en el README.
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

// =============================================================================
//  Firmas (Minecraft Bedrock GDK 1.26.63)
// =============================================================================
// Los ?? son direcciones o desplazamientos que pueden cambiar entre versiones.

// --- Ventana: agarrar ------------------------------------------------------------
// push rsi/rdi/rbx; sub rsp,60h; mov rsi,rcx; (cookie de seguridad);
// cmp byte [rcx+?],0; jne ...; xor ecx,ecx; call [ShowCursor]; ...
constexpr const char* kGrabPattern =
    "56 57 53 48 83 EC 60 48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 58 "
    "80 B9 ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 31 C9 FF 15 ?? ?? ?? ?? 85 C0 78 ?? 48 8B 3D";

// --- Ventana: soltar -------------------------------------------------------------
// ...; xor ecx,ecx; call [ClipCursor]; call [ReleaseCapture];
// mov byte [rsi+OFFSET],0   <- OFFSET = indicador "mouse agarrado" de la ventana
constexpr const char* kReleasePattern =
    "56 57 48 83 EC 28 48 89 CE B9 01 00 00 00 FF 15 ?? ?? ?? ?? 85 C0 79 ?? 48 8B 3D "
    "?? ?? ?? ?? 90 B9 01 00 00 00 FF D7 85 C0 78 ?? 31 C9 FF 15 ?? ?? ?? ?? FF 15 "
    "?? ?? ?? ?? C6 86 ?? ?? ?? ?? 00";
constexpr std::size_t kGrabbedOffsetPosition = 59;

// --- WndProc del juego: su "contexto" global ---------------------------------------
//     mov r15, [rip + X]      ; 4C 8B 3D <X>
constexpr const char* kWndProcPattern =
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 38 01 00 00 48 8D AC 24 80 00 00 00 "
    "48 C7 85 B0 00 00 00 FE FF FF FF 4C 8B 3D ?? ?? ?? ?? 4D 85 FF 0F 95 C0 83 FA 01";
constexpr std::size_t kContextDisplacementPosition = 41;
constexpr std::size_t kContextNextInstruction = 45;

// --- Cliente: agarrar (juego+0x8BF820 en 1.26.63) ----------------------------------
// push rbp/rsi/rdi/rbx; sub rsp,48h; ...; cmp byte [rcx+FLAG],0; jne fin;
// mov rsi,rcx; add rcx,30h ...   (después pone [rsi+FLAG]=1 y llama a la ventana)
constexpr const char* kClientGrabPattern =
    "55 56 57 53 48 83 EC 48 48 8D 6C 24 40 48 C7 45 00 FE FF FF FF 80 B9 ?? ?? ?? ?? 00 "
    "0F 85 ?? ?? ?? ?? 48 89 CE 48 83 C1 30";
constexpr std::size_t kClientGrabFlagPosition = 23;  // los 4 bytes de FLAG en "cmp byte [rcx+FLAG]"

// --- Cliente: soltar (juego+0x8BFA10 en 1.26.63) -----------------------------------
// cmp byte [rcx+FLAG],1; jne fin; mov byte [rcx+FLAG],0; ...;
// mov rcx,[rcx+0x180]; jmp [vtable+...]   (llama a "soltar" de la ventana)
constexpr const char* kClientReleasePattern =
    "80 B9 ?? ?? ?? ?? 01 75 ?? C6 81 ?? ?? ?? ?? 00 48 8B 81 ?? ?? ?? ?? 48 85 C0 74 ?? "
    "80 38 00 75 ?? C7 04 25 00 00 00 00 DE C0 AD DE 48 8B 89";
constexpr std::size_t kClientReleaseFlagPosition = 2;

// =============================================================================
//  Estado
// =============================================================================

// Todas estas funciones reciben un solo parámetro (rcx): el objeto. Devolvemos
// lo que devuelvan, por si quien las llama lo usa.
using MouseFn = std::uintptr_t(__fastcall*)(void* object);

// Ventana.
MouseFn g_originalGrab = nullptr;
MouseFn g_originalRelease = nullptr;
std::uint32_t g_grabbedOffset = 0;    // ventana + esto = "mouse agarrado"
std::uint32_t g_hwndOffset = 0;       // ventana + esto = HWND
std::uintptr_t g_contextGlobal = 0;   // variable global del WndProc
std::atomic<void*> g_window{nullptr};

// Cliente.
MouseFn g_originalClientGrab = nullptr;
MouseFn g_originalClientRelease = nullptr;
std::uint32_t g_clientFlagOffset = 0;  // cliente + esto = "mouse agarrado"
std::atomic<void*> g_client{nullptr};  // lo vemos la primera vez que el juego llama
bool g_clientHooked = false;

std::atomic<bool> g_active{false};
bool g_usedClient = false;        // al abrir el menú, ¿usamos el camino del cliente?
bool g_grabOnClose = false;       // ¿devolverle el mouse al juego al cerrar el menú?
bool g_releasedByUs = false;
bool g_warnedNoObject = false;

// Lee un byte / un puntero de la memoria del juego sin riesgo de crashear
// (__try/__except atrapa la excepción si la dirección no fuera válida).
bool SafeReadByte(const void* object, std::uint32_t offset, std::uint8_t& out) {
    __try {
        out = *(reinterpret_cast<const volatile std::uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadPointer(std::uintptr_t address, std::uintptr_t& out) {
    __try {
        out = *reinterpret_cast<volatile std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsFlagSet(const void* object, std::uint32_t offset) {
    std::uint8_t value = 0;
    return SafeReadByte(object, offset, value) && value != 0;
}

// Busca el objeto de la ventana a partir del contexto del WndProc: es el
// que tiene guardado el HWND del juego en g_hwndOffset. En el juego 1.26.63
// está en [[contexto + 0x8] + 0x78] (medido leyendo la memoria del juego),
// pero buscamos a 1 y 2 niveles para que una actualización no lo rompa.
void* FindWindowObject(HWND gameWindow) {
    if (g_contextGlobal == 0 || g_hwndOffset == 0 || gameWindow == nullptr) {
        return nullptr;
    }
    std::uintptr_t context = 0;
    if (!SafeReadPointer(g_contextGlobal, context) || context == 0) {
        return nullptr;
    }
    const auto target = reinterpret_cast<std::uintptr_t>(gameWindow);
    auto matches = [&](std::uintptr_t object) {
        std::uintptr_t hwnd = 0;
        return object > 0x10000 && SafeReadPointer(object + g_hwndOffset, hwnd) && hwnd == target;
    };
    if (matches(context)) {
        return reinterpret_cast<void*>(context);
    }
    for (std::uintptr_t a = 0; a < 0x400; a += 8) {
        std::uintptr_t object = 0;
        if (SafeReadPointer(context + a, object) && matches(object)) {
            log::Info("SDK: objeto de la ventana encontrado en [contexto+0x{:X}].", a);
            return reinterpret_cast<void*>(object);
        }
    }
    for (std::uintptr_t a = 0; a < 0x200; a += 8) {
        std::uintptr_t first = 0;
        if (!SafeReadPointer(context + a, first) || first < 0x10000) {
            continue;
        }
        for (std::uintptr_t b = 0; b < 0x400; b += 8) {
            std::uintptr_t object = 0;
            if (!SafeReadPointer(first + b, object)) {
                break;
            }
            if (matches(object)) {
                log::Info("SDK: objeto de la ventana encontrado en [[contexto+0x{:X}]+0x{:X}].", a, b);
                return reinterpret_cast<void*>(object);
            }
        }
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        log::Warn("SDK: no se encontró el objeto de la ventana en el contexto del WndProc.");
    }
    return nullptr;
}

// =============================================================================
//  Detours
// =============================================================================

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
    if (g_active.load() && render::overlay::IsMenuOpen() && !g_usedClient) {
        // El juego mismo soltó el mouse (por ejemplo, la ventana perdió el
        // foco): al cerrar el menú NO hay que volver a agarrarlo.
        g_grabOnClose = false;
    }
    return g_originalRelease(window);
}

void RememberClient(void* client) {
    void* expected = nullptr;
    if (g_client.compare_exchange_strong(expected, client)) {
        log::Info("SDK: objeto del cliente capturado (0x{:X}).", reinterpret_cast<std::uintptr_t>(client));
    } else {
        g_client.store(client);
    }
}

std::uintptr_t __fastcall ClientGrabDetour(void* client) {
    hooks::ActiveCallGuard guard;
    RememberClient(client);
    if (g_active.load() && render::overlay::IsMenuOpen()) {
        // Con el menú de Luma abierto, el juego NO puede volver a agarrar el
        // mouse (si no, los clics volverían a ser ataques). Se lo damos al
        // cerrar el menú.
        g_grabOnClose = true;
        return 0;
    }
    return g_originalClientGrab(client);
}

std::uintptr_t __fastcall ClientReleaseDetour(void* client) {
    hooks::ActiveCallGuard guard;
    RememberClient(client);
    if (g_active.load() && render::overlay::IsMenuOpen() && g_usedClient) {
        // El juego soltó el mouse por su cuenta (pausa, foco perdido...):
        // al cerrar el menú no hay que agarrarlo.
        g_grabOnClose = false;
    }
    return g_originalClientRelease(client);
}

bool HookFunction(std::uintptr_t address, void* detour, void** original, const char* name) {
    void* target = reinterpret_cast<void*>(address);
    return hooks::Create(target, detour, original, name) && hooks::Enable(target, name);
}

}  // namespace

// =============================================================================
//  Funciones públicas
// =============================================================================

bool Init(ProgressFn progress) {
    constexpr int kSteps = 5;
    const std::int64_t start = perf::Now();
    if (progress) progress(0, kSteps);
    const ScanResult grab = FindPattern(kGrabPattern);
    if (progress) progress(1, kSteps);
    const ScanResult release = FindPattern(kReleasePattern);
    if (progress) progress(2, kSteps);
    const ScanResult wndProc = FindPattern(kWndProcPattern);
    if (progress) progress(3, kSteps);
    const ScanResult clientGrab = FindPattern(kClientGrabPattern);
    if (progress) progress(4, kSteps);
    const ScanResult clientRelease = FindPattern(kClientReleasePattern);
    if (progress) progress(5, kSteps);
    const double ms = perf::TicksToMs(perf::Now() - start);

    if (wndProc.matches == 1) {
        g_contextGlobal = ResolveRipRelative(wndProc.address + kContextDisplacementPosition,
                                             wndProc.address + kContextNextInstruction);
    }

    // --- Ventana (necesario) ---------------------------------------------------
    if (grab.matches != 1 || release.matches != 1) {
        log::Warn("SDK: firmas del mouse de la ventana no encontradas (agarrar: {}, soltar: {}). "
                  "Con el menú abierto, la cámara seguirá moviéndose. ¿El juego se actualizó?",
                  grab.matches, release.matches);
        notify::Push(notify::Kind::Warning, "Firma no encontrada: el menú no bloqueará el mouse");
        return false;
    }
    g_grabbedOffset = *reinterpret_cast<const std::uint32_t*>(release.address + kGrabbedOffsetPosition);
    if (g_grabbedOffset < 0x10 || g_grabbedOffset > 0x10000) {
        log::Warn("SDK: desplazamiento \"mouse agarrado\" de la ventana sospechoso (0x{:X}).", g_grabbedOffset);
        return false;
    }
    // HWND dentro del objeto: "agarrar" lo lee con mov rcx,[rsi+OFFSET] (48 8B 8E).
    for (std::uintptr_t p = grab.address; p < grab.address + 0x100; ++p) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(p);
        if (bytes[0] == 0x48 && bytes[1] == 0x8B && bytes[2] == 0x8E) {
            g_hwndOffset = *reinterpret_cast<const std::uint32_t*>(p + 3);
            break;
        }
    }
    if (!HookFunction(grab.address, reinterpret_cast<void*>(&GrabDetour),
                      reinterpret_cast<void**>(&g_originalGrab), "juego: ventana agarra el mouse") ||
        !HookFunction(release.address, reinterpret_cast<void*>(&ReleaseDetour),
                      reinterpret_cast<void**>(&g_originalRelease), "juego: ventana suelta el mouse")) {
        return false;
    }

    // --- Cliente (lo que bloquea los clics) ---------------------------------------
    if (clientGrab.matches == 1 && clientRelease.matches == 1) {
        const auto flagFromGrab =
            *reinterpret_cast<const std::uint32_t*>(clientGrab.address + kClientGrabFlagPosition);
        const auto flagFromRelease =
            *reinterpret_cast<const std::uint32_t*>(clientRelease.address + kClientReleaseFlagPosition);
        // Las dos funciones tienen que hablar del MISMO indicador: si no
        // coinciden, alguna firma encontró algo equivocado y no la usamos.
        if (flagFromGrab == flagFromRelease && flagFromGrab > 0x10 && flagFromGrab < 0x10000) {
            g_clientFlagOffset = flagFromGrab;
            g_clientHooked =
                HookFunction(clientGrab.address, reinterpret_cast<void*>(&ClientGrabDetour),
                             reinterpret_cast<void**>(&g_originalClientGrab), "juego: cliente agarra el mouse") &&
                HookFunction(clientRelease.address, reinterpret_cast<void*>(&ClientReleaseDetour),
                             reinterpret_cast<void**>(&g_originalClientRelease), "juego: cliente suelta el mouse");
        } else {
            log::Warn("SDK: las firmas del cliente no coinciden entre sí (0x{:X} / 0x{:X}); no se usan.",
                      flagFromGrab, flagFromRelease);
        }
    } else {
        log::Warn("SDK: firmas del cliente no encontradas (agarrar: {}, soltar: {}). El menú bloqueará "
                  "la cámara pero no los clics.", clientGrab.matches, clientRelease.matches);
    }

    g_active.store(true);
    log::Info("SDK: firmas encontradas en {:.0f} ms. Ventana: agarrar juego+0x{:X}, soltar juego+0x{:X}, "
              "indicador +0x{:X}, HWND +0x{:X}. Cliente: {} (indicador +0x{:X}).",
              ms, grab.address - GameBase(), release.address - GameBase(), g_grabbedOffset, g_hwndOffset,
              g_clientHooked ? "sí" : "no", g_clientFlagOffset);
    return true;
}

bool LocateWindow(HWND gameWindow) {
    if (!g_active.load()) {
        return false;
    }
    if (g_window.load() != nullptr) {
        return true;
    }
    void* window = FindWindowObject(gameWindow);
    if (window == nullptr) {
        return false;
    }
    void* expected = nullptr;
    g_window.compare_exchange_strong(expected, window);
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
    g_usedClient = false;
    g_grabOnClose = false;
    if (!g_active.load()) {
        return;
    }

    // 1. Camino principal: el cliente. Su "soltar" pone el indicador en 0 y
    //    además suelta el mouse de la ventana.
    void* client = g_client.load();
    if (g_clientHooked && client != nullptr) {
        g_usedClient = true;
        g_grabOnClose = IsFlagSet(client, g_clientFlagOffset);
        if (g_grabOnClose) {
            g_originalClientRelease(client);
            g_releasedByUs = true;
        }
        return;
    }

    // 2. Respaldo: solo la ventana (frena la cámara, pero no los clics).
    void* window = g_window.load();
    if (window == nullptr && LocateWindow(GetForegroundWindow())) {
        window = g_window.load();
    }
    if (window == nullptr || g_clientHooked) {
        // Si el camino del cliente existe pero todavía no conocemos el objeto,
        // basta con que el juego agarre o suelte el mouse una vez.
        if (!g_warnedNoObject) {
            g_warnedNoObject = true;
            log::Info("SDK: todavía no se conoce el objeto del cliente (pulsa Esc dos veces en el juego).");
            notify::Push(notify::Kind::Info, "Para bloquear el mouse: cierra el menú y pulsa Esc dos veces");
        }
        if (window == nullptr) {
            return;
        }
    }
    g_grabOnClose = IsFlagSet(window, g_grabbedOffset);
    if (g_grabOnClose) {
        g_originalRelease(window);
        g_releasedByUs = true;
    }
}

int GameMouseState() {
    if (!g_active.load(std::memory_order_relaxed)) {
        return -1;
    }
    void* client = g_client.load(std::memory_order_relaxed);
    if (g_clientHooked && client != nullptr) {
        return IsFlagSet(client, g_clientFlagOffset) ? 1 : 0;
    }
    void* window = g_window.load(std::memory_order_relaxed);
    if (window != nullptr && g_grabbedOffset != 0) {
        return IsFlagSet(window, g_grabbedOffset) ? 1 : 0;
    }
    return -1;
}

void OnMenuClosed() {
    if (g_active.load() && g_grabOnClose) {
        if (g_usedClient) {
            if (void* client = g_client.load()) {
                g_originalClientGrab(client);  // lo mismo que al volver a jugar
            }
        } else if (void* window = g_window.load()) {
            g_originalGrab(window);
        }
    }
    g_grabOnClose = false;
    g_usedClient = false;
    g_releasedByUs = false;
}

void BeginUnload() {
    if (g_active.load() && render::overlay::IsMenuOpen()) {
        OnMenuClosed();
    }
    g_active.store(false);
}

void* Client() {
    return g_client.load(std::memory_order_relaxed);
}

}  // namespace luma::sdk::mouse
