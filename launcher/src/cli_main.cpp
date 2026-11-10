// =============================================================================
//  cli_main.cpp - LumaLauncherCli.exe (la versión de consola, para pruebas)
// =============================================================================
//  Pasos que sigue el launcher:
//    1. Ubicar Luma.dll (tiene que estar en la misma carpeta que el launcher).
//    2. Encontrar el proceso Minecraft.Windows.exe.
//    3. Comprobar que es la versión GDK (no la vieja UWP).
//    4. Comprobar que Luma no esté ya cargado.
//    5. Inyectar Luma.dll.
//    6. Confirmar que Luma escribió su log.
//
//  Opciones de línea de comandos:
//    --no-pause         No esperar a que se pulse Enter al terminar.
//    --skip-gdk-check   Saltar la comprobación de GDK (solo si la detección
//                       falla en tu equipo y estás seguro de que es GDK).
//
//  Código de salida: 0 = todo bien, 1 = error. (Útil para scripts.)
// =============================================================================

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "injector.h"
#include "game_process.h"

namespace fs = std::filesystem;  // Abreviatura para no escribir el nombre largo.
using namespace luma::launcher;

namespace {

constexpr std::wstring_view kGameExe = L"Minecraft.Windows.exe";
constexpr std::wstring_view kDllName = L"Luma.dll";
// Archivo que traen los juegos publicados con el GDK de Microsoft, junto al .exe.
constexpr std::wstring_view kGdkConfigFile = L"MicrosoftGame.config";

bool g_pause = true;

// --- Salida por consola --------------------------------------------------------
// Pequeñas funciones para que los mensajes tengan un formato uniforme.
void Info(const std::string& text) { std::printf("  %s\n", text.c_str()); }
void Ok(const std::string& text) { std::printf("  [OK] %s\n", text.c_str()); }
void Warn(const std::string& text) { std::printf("  [AVISO] %s\n", text.c_str()); }

// Muestra un error, espera Enter (si corresponde) y devuelve el código 1.
int Fail(const std::string& text) {
    std::printf("\n  [ERROR] %s\n", text.c_str());
    if (g_pause) {
        std::printf("\n  Pulsa Enter para cerrar...");
        std::getchar();
    }
    return 1;
}

std::string U8(const fs::path& path) { return ToUtf8(path.native()); }

// Carpeta donde está LumaLauncher.exe.
fs::path LauncherDir() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, length)).parent_path();
}

// %APPDATA%\Luma\logs (la misma carpeta que usa la DLL).
fs::path LogsDir() {
    PWSTR raw = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        dir = fs::path(raw) / L"Luma" / L"logs";
    }
    CoTaskMemFree(raw);
    return dir;
}

// Busca un archivo de log de Luma modificado después de "since".
// Espera hasta ~3 segundos porque la DLL arranca en un hilo propio y puede
// tardar un instante en escribirlo.
fs::path WaitForNewLog(const fs::path& dir, fs::file_time_type since) {
    std::error_code ec;
    for (int attempt = 0; attempt < 30; ++attempt) {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == L".log" && entry.last_write_time(ec) >= since) {
                return entry.path();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return {};
}

}  // namespace

// wmain es como main, pero recibe los argumentos como texto Unicode.
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);  // Para que las tildes se vean bien.

    bool skipGdkCheck = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--no-pause") == 0) {
            g_pause = false;
        } else if (std::wcscmp(argv[i], L"--skip-gdk-check") == 0) {
            skipGdkCheck = true;
        }
    }

    std::printf("\n  Luma Launcher 0.1.0\n  -------------------\n\n");

    // --- 1. Ubicar Luma.dll -------------------------------------------------------
    const fs::path dllPath = LauncherDir() / kDllName;
    std::error_code ec;
    if (!fs::exists(dllPath, ec)) {
        return Fail("No se encontró Luma.dll junto al launcher.\n  Se buscó en: " + U8(dllPath) +
                    "\n  Compila el proyecto o copia Luma.dll a esa carpeta.");
    }
    Ok("Luma.dll encontrada: " + U8(dllPath));

    // --- 2. Encontrar el juego ----------------------------------------------------
    const std::vector<DWORD> pids = FindProcessesByName(kGameExe);
    if (pids.empty()) {
        return Fail("Minecraft no está abierto.\n  Abre Minecraft Bedrock y vuelve a ejecutar el launcher.");
    }
    if (pids.size() > 1) {
        Warn("Hay " + std::to_string(pids.size()) +
             " copias de Minecraft abiertas; se usará la primera.");
    }
    const DWORD pid = pids.front();
    Ok("Minecraft encontrado (PID " + std::to_string(pid) + ").");

    // Abrimos el proceso solo para consultar información (permiso mínimo).
    UniqueHandle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!query.Valid()) {
        return Fail("No se pudo consultar el proceso del juego: " + ErrorMessage(GetLastError()));
    }
    const std::optional<fs::path> gamePath = GetProcessImagePath(query.Get());
    if (gamePath) {
        Info("Ruta del juego: " + U8(*gamePath));
    }

    // --- 3. ¿Es la versión GDK? ---------------------------------------------------
    if (skipGdkCheck) {
        Warn("Comprobación de GDK omitida (--skip-gdk-check).");
    } else {
        // Señal 1: las apps UWP corren dentro de un AppContainer; GDK no.
        const std::optional<bool> appContainer = IsAppContainer(query.Get());
        if (appContainer.value_or(false)) {
            return Fail(
                "Este Minecraft es la versión UWP (antigua), y Luma solo soporta la versión GDK.\n"
                "  Actualiza Minecraft desde la Microsoft Store o la app Xbox.");
        }
        // Señal 2: los juegos GDK traen MicrosoftGame.config junto al .exe.
        const bool hasGdkConfig =
            gamePath && fs::exists(gamePath->parent_path() / kGdkConfigFile, ec);
        if (!hasGdkConfig) {
            return Fail(
                "No se pudo confirmar que este Minecraft sea la versión GDK\n"
                "  (no se encontró MicrosoftGame.config junto al ejecutable).\n"
                "  Si estás seguro de que es GDK, ejecuta el launcher con --skip-gdk-check\n"
                "  y avisa para corregir la detección.");
        }
        Ok("Versión GDK confirmada.");
    }

    // --- 4. ¿Luma ya está cargado? -----------------------------------------------
    if (IsModuleLoaded(pid, kDllName).value_or(false)) {
        return Fail(
            "Luma ya está cargado en este Minecraft.\n"
            "  Pulsa Fin (End) dentro del juego para descargarlo antes de volver a inyectar.\n"
            "  (Si acabas de descargarlo, espera un segundo y vuelve a intentarlo.)");
    }

    // --- 5. Inyectar ------------------------------------------------------------
    const fs::path logsDir = LogsDir();
    const fs::file_time_type injectTime = fs::file_time_type::clock::now();

    Info("Inyectando Luma...");
    const InjectResult result = InjectDll(pid, dllPath);
    if (!result.ok) {
        return Fail(result.error);
    }
    Ok("Luma.dll cargada en el juego.");

    // --- 6. Confirmar que Luma arrancó (escribió su log) ------------------------
    const fs::path log = logsDir.empty() ? fs::path{} : WaitForNewLog(logsDir, injectTime);
    if (!log.empty()) {
        Ok("Luma arrancó. Log: " + U8(log));
    } else {
        Warn("Luma se cargó, pero no se encontró su log en " + U8(logsDir) +
             ".\n  Puede que Windows lo esté guardando en otra carpeta; avisa para revisarlo.");
    }

    std::printf("\n  Listo. Pulsa Fin (End) dentro del juego para descargar Luma.\n");
    if (g_pause) {
        std::printf("\n  Pulsa Enter para cerrar...");
        std::getchar();
    }
    return 0;
}
