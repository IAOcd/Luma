// =============================================================================
//  flow.cpp - Implementación de "JUGAR" (ver flow.h)
// =============================================================================
#include "flow.h"

#include <appmodel.h>  // GetPackagesByPackageFamily: qué versión de Minecraft está instalada
#include <shellapi.h>  // ShellExecuteW: abrir Minecraft
#include <shlobj.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

#include "injector.h"
#include "game_process.h"

namespace fs = std::filesystem;

namespace luma::launcher {

namespace {

constexpr std::wstring_view kGameExe = L"Minecraft.Windows.exe";
constexpr std::wstring_view kDllName = L"Luma.dll";
constexpr std::wstring_view kGdkConfigFile = L"MicrosoftGame.config";
// La "familia" del paquete de Minecraft Bedrock en Windows (la misma desde
// la época UWP: el nombre se conservó al pasar a GDK).
constexpr const wchar_t* kPackageFamily = L"Microsoft.MinecraftUWP_8wekyb3d8bbwe";

// Cuánto esperar a que el juego abra su ventana, y cuánto más después (el
// juego muestra la ventana antes de terminar de arrancar).
constexpr auto kGameOpenTimeout = std::chrono::seconds(120);
constexpr auto kAfterWindowDelay = std::chrono::seconds(6);

fs::path LauncherDir() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, length)).parent_path();
}

fs::path LogsDir() {
    PWSTR raw = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        dir = fs::path(raw) / L"Luma" / L"logs";
    }
    CoTaskMemFree(raw);
    return dir;
}

// ¿El proceso ya tiene una ventana visible con título? (señal de que arrancó)
bool HasVisibleWindow(DWORD pid) {
    struct Search {
        DWORD pid;
        bool found;
    } search{pid, false};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == s->pid && IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0) {
                s->found = true;
                return FALSE;  // dejar de buscar
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Le pide a Windows que abra Minecraft. Primero con el enlace "minecraft:"
// (lo registra el juego al instalarse); si no, por su identificador de app.
bool LaunchGame() {
    const wchar_t* targets[] = {
        L"minecraft:",
        L"shell:AppsFolder\\Microsoft.MinecraftUWP_8wekyb3d8bbwe!Game",
        L"shell:AppsFolder\\Microsoft.MinecraftUWP_8wekyb3d8bbwe!App",
    };
    for (const wchar_t* target : targets) {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL));
        if (result > 32) {  // > 32 = éxito (así lo define ShellExecute)
            return true;
        }
    }
    return false;
}

// Espera un tiempo, pero sale antes si el launcher se está cerrando.
bool SleepUnlessQuitting(const std::atomic<bool>& quitting, std::chrono::milliseconds time) {
    const auto end = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < end) {
        if (quitting.load()) {
            return false;
        }
        Sleep(100);
    }
    return !quitting.load();
}

std::string U8(const fs::path& path) { return ToUtf8(path.native()); }

// --- La DLL que viene dentro del launcher ----------------------------------------
//  launcher/CMakeLists.txt mete Luma.dll en LumaLauncher.exe como "recurso" 2
//  (datos crudos). Para inyectarla tiene que existir como archivo: la copiamos
//  a %APPDATA%\Luma\bin\Luma.dll, solo si cambió (si es idéntica, no se toca).
//  Si el juego tiene cargada una versión vieja, el archivo está "en uso" y no
//  se puede reemplazar: seguimos con el que hay (el paso 3 avisa que Luma ya
//  está cargado y que hay que descargarlo con Fin).
constexpr int kEmbeddedDllResource = 2;

fs::path ExtractEmbeddedDll(std::string& error) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(kEmbeddedDllResource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL loaded = resource != nullptr ? LoadResource(nullptr, resource) : nullptr;
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    const DWORD size = resource != nullptr ? SizeofResource(nullptr, resource) : 0;
    if (data == nullptr || size == 0) {
        error = "(this launcher has no embedded Luma.dll)";
        return {};
    }
    const fs::path dir = LogsDir().parent_path() / L"bin";
    const fs::path target = dir / kDllName;
    std::error_code ec;
    fs::create_directories(dir, ec);

    // ¿Ya está y es igual?
    {
        HANDLE file = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            bool same = false;
            LARGE_INTEGER existingSize{};
            if (GetFileSizeEx(file, &existingSize) && existingSize.QuadPart == size) {
                std::vector<char> existing(size);
                DWORD read = 0;
                same = ReadFile(file, existing.data(), size, &read, nullptr) && read == size &&
                       std::memcmp(existing.data(), data, size) == 0;
            }
            CloseHandle(file);
            if (same) {
                return target;
            }
        }
    }
    // Escribir a un archivo temporal y reemplazar (así nunca queda a medias).
    const fs::path temp = dir / L"Luma.dll.tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = ErrorMessage(GetLastError()) + " (" + U8(temp) + ")";
        return fs::exists(target, ec) ? target : fs::path{};
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = ErrorMessage(GetLastError()) + " (" + U8(target) + ")";
        DeleteFileW(temp.c_str());
        return fs::exists(target, ec) ? target : fs::path{};  // en uso: seguir con la que hay
    }
    return target;
}

}  // namespace

PlayFlow::~PlayFlow() {
    quitting_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void PlayFlow::Start(std::vector<std::string> supportedMinecraft, bool force) {
    if (busy_.exchange(true)) {
        return;  // ya está en marcha
    }
    if (thread_.joinable()) {
        thread_.join();  // el anterior ya terminó
    }
    thread_ = std::thread([this, supported = std::move(supportedMinecraft), force] {
        Run(supported, force);
        busy_.store(false);
    });
}

PlaySnapshot PlayFlow::Snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

bool PlayFlow::Busy() const {
    return busy_.load();
}

void PlayFlow::Set(PlayState state, const char* key, std::string detail) {
    std::lock_guard lock(mutex_);
    snapshot_.state = state;
    snapshot_.messageKey = key;
    snapshot_.detail = std::move(detail);
}

void PlayFlow::Run(const std::vector<std::string>& supported, bool force) {
    std::error_code ec;
    // --- 1. Luma.dll ------------------------------------------------------------------
    // Si hay una Luma.dll junto al launcher (la carpeta de compilación), se usa
    // esa. Si no, la que viene DENTRO del launcher, copiada a %APPDATA%\Luma\bin.
    fs::path dllPath = LauncherDir() / kDllName;
    if (!fs::exists(dllPath, ec)) {
        std::string error;
        dllPath = ExtractEmbeddedDll(error);
        if (dllPath.empty()) {
            Set(PlayState::Failed, "error.no_dll", error);
            return;
        }
    }

    // --- 2. El juego: abrirlo si hace falta --------------------------------------------
    std::vector<DWORD> pids = FindProcessesByName(kGameExe);
    if (pids.empty()) {
        Set(PlayState::LaunchingGame, "play.launching");
        if (!LaunchGame()) {
            Set(PlayState::Failed, "error.launch_failed");
            return;
        }
        Set(PlayState::WaitingForGame, "play.waiting");
        const auto deadline = std::chrono::steady_clock::now() + kGameOpenTimeout;
        bool ready = false;
        while (!ready && std::chrono::steady_clock::now() < deadline) {
            if (!SleepUnlessQuitting(quitting_, std::chrono::milliseconds(500))) {
                return;
            }
            pids = FindProcessesByName(kGameExe);
            ready = !pids.empty() && HasVisibleWindow(pids.front());
        }
        if (!ready) {
            Set(PlayState::Failed, "error.game_timeout");
            return;
        }
        if (!SleepUnlessQuitting(quitting_, kAfterWindowDelay)) {
            return;
        }
        pids = FindProcessesByName(kGameExe);
        if (pids.empty()) {
            Set(PlayState::Failed, "error.game_closed");
            return;
        }
    }
    const DWORD pid = pids.front();

    // --- 3. ¿Es GDK? ¿Luma ya está? ---------------------------------------------------
    UniqueHandle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!query.Valid()) {
        Set(PlayState::Failed, "error.query", ErrorMessage(GetLastError()));
        return;
    }
    if (IsAppContainer(query.Get()).value_or(false)) {
        Set(PlayState::Failed, "error.uwp");
        return;
    }
    const std::optional<fs::path> gamePath = GetProcessImagePath(query.Get());
    if (!(gamePath && fs::exists(gamePath->parent_path() / kGdkConfigFile, ec))) {
        Set(PlayState::Failed, "error.not_gdk", gamePath ? U8(*gamePath) : std::string{});
        return;
    }
    if (IsModuleLoaded(pid, kDllName).value_or(false)) {
        Set(PlayState::AlreadyLoaded, "play.already");
        return;
    }
    // ¿Esta versión de Minecraft está soportada? (las direcciones internas del
    // juego cambian en cada actualización: con una versión nueva, algunos
    // módulos pueden quedar desactivados hasta que salga una Luma nueva)
    if (!force && !supported.empty()) {
        const std::string version = QueryGameStatus().gameVersion;
        bool ok = version.empty();  // si no se sabe la versión, no molestar
        for (const std::string& s : supported) {
            ok |= s == version;
        }
        if (!ok) {
            Set(PlayState::Unsupported, "play.unsupported", version);
            return;
        }
    }

    // --- 4. Inyectar y confirmar -----------------------------------------------------
    Set(PlayState::Injecting, "play.injecting");
    const fs::path logsDir = LogsDir();
    const fs::file_time_type injectTime = fs::file_time_type::clock::now();
    const InjectResult result = InjectDll(pid, dllPath);
    if (!result.ok) {
        Set(PlayState::Failed, "error.inject", result.error);
        return;
    }
    Set(PlayState::Verifying, "play.verifying");
    for (int attempt = 0; attempt < 50 && !logsDir.empty(); ++attempt) {
        for (const auto& entry : fs::directory_iterator(logsDir, ec)) {
            if (entry.path().extension() == L".log" && entry.last_write_time(ec) >= injectTime) {
                Set(PlayState::Done, "play.done");
                return;
            }
        }
        if (!SleepUnlessQuitting(quitting_, std::chrono::milliseconds(100))) {
            return;
        }
    }
    Set(PlayState::Done, "play.done_no_log", U8(logsDir));
}

GameStatus QueryGameStatus() {
    GameStatus status;
    const std::vector<DWORD> pids = FindProcessesByName(kGameExe);
    status.running = !pids.empty();
    status.lumaLoaded = status.running && IsModuleLoaded(pids.front(), kDllName).value_or(false);

    // Versión instalada: el nombre completo del paquete la trae, por ejemplo
    // "Microsoft.MinecraftUWP_1.26.6301.0_x64__8wekyb3d8bbwe" -> 1.26.6301.0,
    // que el juego muestra como 1.26.63.
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    if (GetPackagesByPackageFamily(kPackageFamily, &count, nullptr, &bufferLength, nullptr) ==
            ERROR_INSUFFICIENT_BUFFER &&
        count > 0) {
        std::vector<PWSTR> names(count);
        std::vector<wchar_t> buffer(bufferLength);
        if (GetPackagesByPackageFamily(kPackageFamily, &count, names.data(), &bufferLength, buffer.data()) ==
                ERROR_SUCCESS &&
            count > 0) {
            unsigned major = 0, minor = 0, build = 0, revision = 0;
            const wchar_t* underscore = std::wcschr(names[0], L'_');
            if (underscore != nullptr &&
                swscanf_s(underscore + 1, L"%u.%u.%u.%u", &major, &minor, &build, &revision) == 4) {
                char text[32];
                std::snprintf(text, sizeof(text), "%u.%u.%u", major, minor, build >= 100 ? build / 100 : build);
                status.gameVersion = text;
            }
        }
    }
    return status;
}

}  // namespace luma::launcher
