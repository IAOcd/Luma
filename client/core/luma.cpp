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

#include "core/logger.h"
#include "core/paths.h"
#include "core/version.h"

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

    // --- Aquí se irán agregando los sistemas en las próximas fases ---------
    // Fase 2: hook de Present + ImGui.
    // Fase 3: módulos y configuración.

    log::Info("Listo. Pulsa Fin (End) con el juego en primer plano para descargar Luma.");
    return true;
}

void WaitForUnload() {
    // GetAsyncKeyState devuelve un número cuyo bit más alto (0x8000) vale 1
    // si la tecla está apretada en este momento.
    while (!(IsGameWindowFocused() && (GetAsyncKeyState(kUnloadKey) & 0x8000))) {
        Sleep(kPollIntervalMs);
    }
    log::Info("Tecla de descarga pulsada.");
}

void Shutdown() {
    // Orden INVERSO al de Startup.
    // Fase 2+: quitar hooks primero, después liberar ImGui, etc.

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
