// =============================================================================
//  logger.cpp - Implementación del log
// =============================================================================
#include "core/logger.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <share.h>  // _SH_DENYNO: permitir que otros programas lean el archivo
#include <string>
#include <system_error>
#include <vector>

namespace luma::log {

// Un "namespace" sin nombre hace que todo lo de adentro sea privado de este
// archivo: nadie más puede tocar estas variables.
namespace {

// Cuántos archivos de log se conservan (el actual incluido).
constexpr std::size_t kMaxLogFiles = 10;
constexpr std::wstring_view kLogPrefix = L"luma_";
constexpr std::wstring_view kLogExtension = L".log";

// Un "mutex" es un candado: si dos hilos quieren escribir en el log al mismo
// tiempo, uno espera a que el otro termine. Sin esto, las líneas podrían
// mezclarse entre sí.
std::mutex g_mutex;
std::FILE* g_file = nullptr;
std::filesystem::path g_filePath;
HANDLE g_consoleOut = nullptr;  // salida de la consola, si está abierta

// --- Consola -----------------------------------------------------------------

// Windows avisa a los programas de consola cuando el usuario pulsa Ctrl+C.
// Por defecto, eso CIERRA EL PROCESO... que en nuestro caso es el juego.
// Este "manejador" le responde a Windows "ya lo manejé" (TRUE) y no hace nada.
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ? TRUE : FALSE;
}

void OpenConsole() {
    if (!AllocConsole()) {
        return;  // Ya había una consola o Windows no quiso: seguimos sin ella.
    }
    SetConsoleTitleW(L"Luma - consola de depuración");
    SetConsoleOutputCP(CP_UTF8);  // Para que tildes y ñ se vean bien.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Cerrar la ventana de consola con la X también cerraría el juego entero.
    // Para evitarlo, quitamos el botón de cerrar de esa ventana.
    if (HWND window = GetConsoleWindow()) {
        if (HMENU menu = GetSystemMenu(window, FALSE)) {
            DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
        }
    }

    g_consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
}

void CloseConsole() {
    if (g_consoleOut == nullptr) {
        return;
    }
    // MUY IMPORTANTE: quitar el manejador antes de descargar la DLL. Si no,
    // Windows seguiría apuntando a una función que ya no existe en memoria y
    // el próximo Ctrl+C crashearía el juego.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    FreeConsole();
    g_consoleOut = nullptr;
}

// --- Archivos ------------------------------------------------------------------

// Hora local en el formato pedido. "forFileName" usa guiones en lugar de ":"
// porque Windows no permite ":" en nombres de archivo.
std::string Timestamp(bool forFileName) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    if (forFileName) {
        return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay,
                           t.wHour, t.wMinute, t.wSecond);
    }
    return std::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond,
                       t.wMilliseconds);
}

// Borra los logs más viejos para que, al crear el nuevo, queden kMaxLogFiles.
// Como el nombre empieza con la fecha en formato año-mes-día, ordenar por
// nombre es lo mismo que ordenar por antigüedad.
void DeleteOldLogs(const std::filesystem::path& dir) {
    // Usamos la versión de las funciones de filesystem que recibe un
    // "error_code": si algo falla, nos lo dice en esa variable en vez de
    // lanzar una excepción. Dentro del juego preferimos no lanzar nunca.
    std::error_code ec;
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.starts_with(kLogPrefix) && name.ends_with(kLogExtension)) {
            logs.push_back(entry.path());
        }
    }
    if (logs.size() < kMaxLogFiles) {
        return;
    }
    std::sort(logs.begin(), logs.end());
    // Dejamos kMaxLogFiles - 1 para hacerle lugar al archivo nuevo.
    const std::size_t toDelete = logs.size() - (kMaxLogFiles - 1);
    for (std::size_t i = 0; i < toDelete; ++i) {
        std::filesystem::remove(logs[i], ec);
    }
}

const char* LevelName(Level level) {
    switch (level) {
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

bool Init(const std::filesystem::path& logDir, bool withConsole) {
    // "lock_guard" toma el candado y lo suelta solo al salir de la función.
    std::lock_guard lock(g_mutex);

    if (withConsole) {
        OpenConsole();
    }
    if (logDir.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);  // no falla si ya existe
    DeleteOldLogs(logDir);

    const std::string stamp = Timestamp(true);
    g_filePath = logDir / (std::wstring(kLogPrefix) +
                           std::wstring(stamp.begin(), stamp.end()) +
                           std::wstring(kLogExtension));

    // _wfsopen abre el archivo permitiendo que otros programas (por ejemplo,
    // el Bloc de notas) lo lean mientras el juego está abierto.
    // "ab" = agregar al final, en modo binario (escribimos el texto tal cual).
    g_file = _wfsopen(g_filePath.c_str(), L"ab", _SH_DENYNO);
    if (g_file == nullptr) {
        g_filePath.clear();
        return false;
    }
    return true;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    CloseConsole();
}

void Write(Level level, std::string_view message) {
    // Armamos la línea completa fuera del candado, así el candado se mantiene
    // tomado el menor tiempo posible.
    const std::string line =
        std::format("[{}] [{}] {}\n", Timestamp(false), LevelName(level), message);

    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        // fflush fuerza a escribir en disco YA. Si el juego crashea un
        // instante después, la línea igual queda guardada.
        std::fflush(g_file);
    }
    if (g_consoleOut != nullptr) {
        DWORD written = 0;
        WriteConsoleA(g_consoleOut, line.data(), static_cast<DWORD>(line.size()), &written,
                      nullptr);
    }
    // También al "depurador" de Windows (se ve con herramientas como DebugView).
    OutputDebugStringA(line.c_str());
}

const std::filesystem::path& CurrentFile() {
    return g_filePath;
}

}  // namespace luma::log
