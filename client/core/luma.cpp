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

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <format>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/hooks.h"
#include "core/keys.h"
#include "core/logger.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/version.h"
#include "modules/module_manager.h"
#include "modules/performance/process_opts.h"
#include "render/notifications.h"
#include "render/sound.h"
#include "perf/benchmark.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/dx12_hooks.h"
#include "render/input.h"
#include "render/item_icons.h"
#include "render/assets.h"
#include "render/loading.h"
#include "render/overlay.h"
#include "sdk/mouse_grab.h"
#include "sdk/mouse_queue.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"
#include "sdk/paperdoll.h"
#include "core/stats.h"
#include "sdk/network.h"
#include "sdk/player.h"

namespace luma {

namespace {

// La tecla para descargar Luma (por defecto Fin/End) está en
// config::g_unloadKey y se cambia desde Ajustes.

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

// ¿Se cargó la config? (Si no, al descargar NO se guarda, para no pisar el
// archivo con valores vacíos.)
bool g_configLoaded = false;

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

    // Desde aquí, la pantalla de carga muestra cada paso (el overlay ya
    // dibuja en cuanto llega el primer frame, en paralelo con este hilo).
    render::loading::Begin();
    render::assets::Decode();  // el logo (se sube a la GPU en el próximo frame)
    render::loading::SetStep("Cargando la configuración...", 0.10f);

    // --- Módulos y configuración (Fase 3) --------------------------------------
    // Primero se crean los módulos (todos desactivados) y después la config
    // activa los que estaban activos y les pone sus opciones y teclas.
    {
        std::lock_guard lock(StateMutex());
        modules::Manager::RegisterAll();
    }
    config::LoadAtStartup();
    g_configLoaded = true;
    sound::Init();  // después de la config: usa el volumen guardado
    log::Info("{} módulos registrados. Perfil activo: {}.", modules::Manager::All().size(),
              config::ActiveProfile());

    // --- SDK (memoria del juego) -------------------------------------------------
    // Las funciones con las que el juego agarra y suelta el mouse. Si no se
    // encuentran, solo se pierde el bloqueo de la cámara con el menú abierto.
    if (g_hooksReady) {
        sdk::mouse::Init([](int step, int total) {
            static const char* const kSteps[] = {"Buscando funciones del juego (1/5)...",
                                                 "Buscando funciones del juego (2/5)...",
                                                 "Buscando funciones del juego (3/5)...",
                                                 "Buscando funciones del juego (4/5)...",
                                                 "Buscando funciones del juego (5/5)...",
                                                 "Conectando con el juego..."};
            render::loading::SetStep(kSteps[std::min(step, 5)],
                                     0.2f + 0.7f * static_cast<float>(step) / static_cast<float>(total));
        });
    }
    // La cola de acciones del mouse: lo que de verdad le impide al juego
    // recibir clics y movimiento con el menú abierto (ver sdk/mouse_queue.h).
    if (g_hooksReady) {
        render::loading::SetStep("Conectando con el mouse del juego...", 0.95f);
        sdk::mousequeue::Init();
        // El jugador local (Fase 6): se captura al mover el mouse en un mundo.
        sdk::player::Init();
        sdk::network::Init();  // ping y detección de servidor
        sdk::camera::Init();   // brillo, perspectiva, cámara (Fase 8)
        sdk::draw3d::Init();   // dibujar dentro del mundo (contorno, hitboxes)
        sdk::paperdoll::Init();  // mover el muñeco del HUD del juego
        std::lock_guard lock(StateMutex());
        modules::Manager::DisableUnavailable();
    }
    render::loading::Finish();
    stats::Begin();  // una sesión más (para el launcher)

    const std::string menuKey = keys::Name(config::g_menuKey.load());
    notify::Push(notify::Kind::Info, std::format("{} cargado · pulsa {} para abrir el menú", kName, menuKey));
    log::Info("Listo. Menú: {}. Descargar: {} (con el juego en primer plano).", menuKey,
              keys::Name(config::g_unloadKey.load()));
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

    bool windowLocated = !g_hooksReady;

    // GetAsyncKeyState devuelve un número cuyo bit más alto (0x8000) vale 1
    // si la tecla está apretada en este momento. La tecla de descarga se lee
    // de la config en cada vuelta (se puede cambiar desde Ajustes).
    while (!(IsGameWindowFocused() &&
             (GetAsyncKeyState(config::g_unloadKey.load(std::memory_order_relaxed)) & 0x8000))) {
        Sleep(kPollIntervalMs);
        // Encontrar el objeto de la ventana del juego en cuanto el juego esté
        // en primer plano (así el menú bloquea la cámara desde el principio).
        if (!windowLocated && IsGameWindowFocused()) {
            windowLocated = sdk::mouse::LocateWindow(GetForegroundWindow()) || !sdk::mouse::Available();
        }
        config::SaveIfDirty();  // guardado automático (solo si hubo cambios)
        render::input::PollHolds();             // toggle sprint/sneak: menús del juego y foco
        perf::benchmark::Poll();
        stats::Poll();                          // tiempo jugado y nombre del jugador (para el launcher)
        render::icons_items::Poll();            // íconos de ítems pedidos por el HUD                // terminar pruebas (cálculos y CSV fuera del render)
        modules::performance::PollRenderThread();  // prioridad del hilo de render
        {
            std::lock_guard lock(StateMutex());
            modules::Manager::Tick();  // módulos que revisan algo cada 50 ms (aviso de durabilidad...)
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
    //  7. Módulos: desactivarlos y borrarlos (ya nadie los dibuja).
    //
    //  Antes de todo, guardamos la config (por si había cambios pendientes).
    if (g_configLoaded) {
        config::SaveNow();
    }
    stats::End();
    sdk::camera::BeginUnload();
    sdk::paperdoll::BeginUnload();  // el muñeco vuelve a su lugar
    sdk::network::BeginUnload();
    sdk::player::BeginUnload();
    sdk::mousequeue::BeginUnload();
    sdk::mouse::BeginUnload();  // si el menú estaba abierto, devolverle el mouse al juego
    render::dx12::BeginUnload();
    hooks::DisableAll();
    hooks::WaitForActiveCalls();
    render::input::Uninstall();
    hooks::WaitForActiveCalls();
    render::overlay::Shutdown();
    hooks::Shutdown();
    if (g_hooksReady) {
        log::Info("Overlay y hooks liberados.");
    }
    {
        std::lock_guard lock(StateMutex());
        modules::Manager::Shutdown();
    }
    notify::Clear();
    sound::Shutdown();

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
