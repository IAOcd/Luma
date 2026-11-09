// =============================================================================
//  luma.cpp - Ciclo de vida de Luma
// =============================================================================
//  Orden de vida de Luma dentro del juego:
//
//    1. Startup()      -> abrir el log, (Fase 2+) instalar hooks, etc.
//    2. WaitForUnload()-> esperar a que el usuario pulse la tecla de descarga.
//    3. Shutdown()     -> deshacer TODO lo del paso 1, en orden inverso.
//
//  La regla de oro: todo lo que se instala en Startup se quita en Shutdown.
//  Si dejáramos un hook puesto y luego la DLL desaparece de la memoria, el
//  juego saltaría a código que ya no existe y crashearía.
// =============================================================================
#include "core/luma.h"

#include <cstdint>
#include <cwchar>
#include <string>

#include "core/hooks.h"
#include "core/logger.h"
#include "core/paths.h"
#include "core/version.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/dx12_hooks.h"
#include "render/gameinput_block.h"
#include "render/input.h"
#include "render/overlay.h"

namespace luma {

namespace {

// Tecla para descargar Luma. VK_END es la tecla "Fin" (End).
// En la Fase 3 será configurable desde el menú.
constexpr int kUnloadKey = VK_END;

// Cada cuánto revisamos la tecla. 50 ms es imperceptible para una persona y
// prácticamente no gasta CPU (el hilo pasa el 99,9% del tiempo dormido).
constexpr DWORD kPollIntervalMs = 50;

// En Debug abrimos una consola para ver el log en vivo. En Release no.
// NDEBUG es una "macro" que el compilador define solo en Release.
#ifdef NDEBUG
constexpr bool kOpenConsole = false;
#else
constexpr bool kOpenConsole = true;
#endif

// Candado con nombre a nivel de Windows (un "mutex" con nombre). Sirve para
// detectar si otra copia de Luma ya está corriendo dentro de este mismo juego.
HANDLE g_instanceMutex = nullptr;

// ¿Se instalaron bien los hooks de DirectX?
bool g_hooksReady = false;

// Intenta "reservar" el nombre Luma para este proceso.
// Devuelve false si otra copia de Luma ya lo tenía.
bool AcquireInstance() {
    // El nombre incluye el ID del proceso (PID), así dos juegos abiertos a la
    // vez no se bloquean entre sí. "Local\" = solo en esta sesión de Windows.
    wchar_t name[64];
    std::swprintf(name, std::size(name), L"Local\\Luma_Instance_%lu", GetCurrentProcessId());

    g_instanceMutex = CreateMutexW(nullptr, FALSE, name);
    if (g_instanceMutex == nullptr) {
        return true;  // No pudimos comprobarlo; seguimos (no es motivo para no cargar).
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return false;
    }
    return true;
}

void ReleaseInstance() {
    if (g_instanceMutex != nullptr) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
}

// ¿La ventana que tiene el foco ahora mismo pertenece a este proceso (el juego)?
// Así la tecla de descarga no se activa si pulsas "Fin" escribiendo en otro
// programa con Minecraft minimizado.
bool IsGameWindowFocused() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

// Ruta del ejecutable del juego, solo para anotarla en el log.
std::string GameExePath() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return paths::ToUtf8(std::wstring(buffer, length));
}

bool Startup(HMODULE module) {
    log::Init(paths::LogsDir(), kOpenConsole);
    log::Info("{} {} cargado (DLL en 0x{:X})", kName, kVersion,
              reinterpret_cast<std::uintptr_t>(module));
    log::Info("Juego: {} (PID {})", GameExePath(), GetCurrentProcessId());
    log::Info("Log: {}", paths::ToUtf8(log::CurrentFile()));

    if (!AcquireInstance()) {
        log::Warn("Ya hay otra copia de Luma activa en este juego; esta copia no hará nada.");
        return false;
    }

    // --- Overlay (Fase 2) ---------------------------------------------------
    // Si algo de esto falla, Luma sigue cargado sin overlay: lo importante es
    // no crashear el juego. El motivo queda en el log.
    perf::Init();
    if (hooks::Init()) {
        const bool dx12 = render::dx12::CreateHooks();
        // Sin estos hooks el menú funciona, pero el juego seguiría moviendo la
        // cámara con el menú abierto. No es motivo para no cargar.
        if (!render::input::CreateHooks()) {
            log::Warn("Hooks de input no disponibles: con el menú abierto, el mouse moverá la cámara.");
        }
        g_hooksReady = dx12 && hooks::EnableAll();
    }
    if (g_hooksReady) {
        log::Info("Hooks de DirectX 12 activos. Esperando el primer frame...");
    } else {
        log::Warn("El overlay no se pudo activar; Luma sigue cargado sin él.");
    }

    // Fase 3: módulos y configuración.

    log::Info("Listo. Pulsa Fin (End) con el juego en primer plano para descargar Luma.");
    return true;
}

// Escribe en el log el resumen de rendimiento de los últimos 30 s.
// Corre en ESTE hilo (no en el de render), así escribir en disco no le
// agrega ningún tirón al juego.
void LogPerformanceSummary() {
    const perf::Summary s = perf::TakeSummary();
    if (!s.valid) {
        return;
    }
    if (s.closedFrames > 0) {
        log::Info("Rendimiento ({:.0f} s): {:.0f} FPS promedio | costo de Luma con el menú cerrado: "
                  "{:.4f} ms promedio, {:.4f} ms máximo ({} frames)",
                  s.seconds, s.averageFps, s.averageCostUs / 1000.0, s.maxCostUs / 1000.0,
                  s.closedFrames);
    } else {
        log::Info("Rendimiento ({:.0f} s): {:.0f} FPS promedio (menú abierto todo el período)",
                  s.seconds, s.averageFps);
    }
}

void WaitForUnload() {
    constexpr ULONGLONG kSummaryEveryMs = 30'000;
    perf::TakeSummary();  // Empezar a contar desde ahora.
    // Usamos el reloj real (GetTickCount64) en vez de sumar "50 ms" por vuelta:
    // Sleep(50) en Windows suele durar algo más (~62 ms), y el error se acumula.
    ULONGLONG lastSummary = GetTickCount64();

    // GetAsyncKeyState devuelve un número cuyo bit más alto (0x8000) vale 1
    // si la tecla está apretada en este momento.
    // El filtro de GameInput necesita una lectura de mouse o teclado, que
    // quizá todavía no exista al cargar Luma: lo intentamos en cada vuelta
    // hasta que quede instalado.
    bool gameInputDone = !g_hooksReady;

    while (!(IsGameWindowFocused() && (GetAsyncKeyState(kUnloadKey) & 0x8000))) {
        Sleep(kPollIntervalMs);
        if (!gameInputDone) {
            gameInputDone = render::gameinput::TryInstall();
        }
        if (GetTickCount64() - lastSummary >= kSummaryEveryMs) {
            lastSummary = GetTickCount64();
            LogPerformanceSummary();
        }
    }
    log::Info("Tecla de descarga pulsada.");
}

void Shutdown() {
    // Orden INVERSO al de Startup. Cada paso depende del anterior:
    //
    //  1. BeginUnload: los detours dejan de tocar nuestro estado y solo
    //     llaman a la función original.
    //  2. DisableAll: MinHook restaura las funciones originales; ya no entra
    //     nadie nuevo a nuestros detours.
    //  3. WaitForActiveCalls: esperar a que salgan los que ya estaban dentro.
    //  4. input::Uninstall: devolver la WndProc original del juego, y esperar
    //     otra vez por si un mensaje estaba pasando por la nuestra.
    //  5. overlay::Shutdown: ya nadie usa ImGui ni DX12 desde el juego; ahora
    //     sí es seguro liberarlos.
    //  6. hooks::Shutdown: liberar MinHook.
    render::dx12::BeginUnload();
    render::gameinput::BeginUnload();
    hooks::DisableAll();
    hooks::WaitForActiveCalls();
    render::input::Uninstall();
    hooks::WaitForActiveCalls();
    render::overlay::Shutdown();
    hooks::Shutdown();
    if (g_hooksReady) {
        log::Info("Overlay y hooks liberados.");
    }

    ReleaseInstance();
    log::Info("{} descargado correctamente.", kName);
    log::Shutdown();  // Siempre lo último: así todo lo anterior queda en el log.
}

}  // namespace

void Run(HMODULE module) {
    if (Startup(module)) {
        WaitForUnload();
    }
    Shutdown();
}

}  // namespace luma
