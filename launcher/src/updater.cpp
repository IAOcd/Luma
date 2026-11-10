// =============================================================================
//  updater.cpp - Actualizaciones automáticas (ver updater.h)
// =============================================================================
#include "updater.h"

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/version.h"
#include "i18n.h"
#include "net.h"
#include "signature.h"

namespace luma::launcher::updater {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Dónde se publican las versiones. "latest/download" siempre apunta a la
// última Release de GitHub.
constexpr const wchar_t* kManifestUrl = L"https://github.com/IAOcd/Luma/releases/latest/download/latest.json";
constexpr const wchar_t* kSignatureUrl = L"https://github.com/IAOcd/Luma/releases/latest/download/latest.json.sig";
// El .exe tiene que venir de las Releases de ESTE repositorio (además de la firma).
constexpr const char* kAllowedDownloadPrefix = "https://github.com/IAOcd/Luma/releases/download/";
constexpr std::uint64_t kMaxExeSize = 64ull * 1024 * 1024;

std::mutex g_mutex;
Info g_info;
json g_manifest;               // el último latest.json verificado (o el guardado)
// Las novedades ya armadas por idioma (con la ruta de su imagen, si ya se bajó).
// Se arman cuando cambia el manifiesto o terminan de bajar imágenes, nunca por frame.
std::map<std::string, std::vector<Note>> g_notes;
std::atomic<bool> g_autoInstall{true};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};
std::thread g_worker;

#ifdef LUMA_OFFICIAL_BUILD
constexpr bool kOfficial = true;
#else
constexpr bool kOfficial = false;
#endif

fs::path DataDir() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma";
    }
    CoTaskMemFree(raw);
    return path;
}

fs::path ExePath() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, length));
}

// "0.2.10" -> {0, 2, 10}. Compara versiones número por número.
bool Newer(const std::string& a, const std::string& b) {
    unsigned va[3]{}, vb[3]{};
    sscanf_s(a.c_str(), "%u.%u.%u", &va[0], &va[1], &va[2]);
    sscanf_s(b.c_str(), "%u.%u.%u", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; ++i) {
        if (va[i] != vb[i]) return va[i] > vb[i];
    }
    return false;
}

void SetState(State state, const char* errorKey = "", std::string detail = {}) {
    std::lock_guard lock(g_mutex);
    g_info.state = state;
    g_info.errorKey = errorKey;
    g_info.detail = std::move(detail);
}

fs::path ImagePath(const std::string& url);
bool ImageAllowed(const std::string& url);

// Arma g_notes (llamar con g_mutex tomado).
void RebuildNotesLocked() {
    g_notes.clear();
    if (!g_manifest.contains("notes") || !g_manifest["notes"].is_object()) return;
    for (const auto& [lang, list] : g_manifest["notes"].items()) {
        if (!list.is_array()) continue;
        std::vector<Note>& out = g_notes[lang];
        for (const json& n : list) {
            if (!n.is_object()) continue;
            Note note{n.value("title", std::string()), n.value("body", std::string()), {}};
            const std::string url = n.value("image", std::string());
            std::error_code ec;
            if (!url.empty() && ImageAllowed(url) && fs::exists(ImagePath(url), ec)) {
                note.image = ImagePath(url).native();
            }
            out.push_back(std::move(note));
        }
    }
}

// Toma lo que la interfaz necesita del latest.json.
void ApplyManifest(const json& manifest) {
    std::lock_guard lock(g_mutex);
    g_manifest = manifest;
    RebuildNotesLocked();
    g_info.latestVersion = manifest.value("version", std::string());
    g_info.date = manifest.value("date", std::string());
    g_info.supportedMinecraft.clear();
    // La lista de versiones de Minecraft solo vale para ESTA versión de Luma.
    if (g_info.latestVersion == kVersion && manifest.contains("minecraft") && manifest["minecraft"].is_array()) {
        for (const json& v : manifest["minecraft"]) {
            if (v.is_string()) g_info.supportedMinecraft.push_back(v.get<std::string>());
        }
    }
}

// Lee la última respuesta guardada (para tener noticias aunque no haya internet).
void LoadCache() {
    std::ifstream file(DataDir() / L"latest.json");
    if (!file) return;
    const json manifest = json::parse(file, nullptr, false);
    if (manifest.is_object()) {
        ApplyManifest(manifest);
    }
}

void SaveCache(const std::string& text) {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream(DataDir() / L"latest.json", std::ios::binary | std::ios::trunc) << text;
}

// --- Imágenes de las noticias (opcionales) ------------------------------------------
//  Cada novedad puede traer "image": la URL de una imagen del repositorio. Se
//  baja una vez a %APPDATA%/Luma/news/ y después se usa la guardada.
constexpr const char* kAllowedImagePrefixes[] = {
    "https://raw.githubusercontent.com/IAOcd/Luma/",
    "https://github.com/IAOcd/Luma/",
};

bool ImageAllowed(const std::string& url) {
    for (const char* prefix : kAllowedImagePrefixes) {
        if (url.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// Nombre del archivo local de una URL (un número a partir del texto: FNV-1a).
fs::path ImagePath(const std::string& url) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : url) h = (h ^ c) * 0x100000001B3ull;
    wchar_t name[32];
    std::swprintf(name, std::size(name), L"%016llx.img", static_cast<unsigned long long>(h));
    return DataDir() / L"news" / name;
}

void DownloadImages(const json& manifest) {
    if (!manifest.contains("notes") || !manifest["notes"].is_object()) return;
    std::error_code ec;
    fs::create_directories(DataDir() / L"news", ec);
    for (const auto& [lang, list] : manifest["notes"].items()) {
        if (!list.is_array()) continue;
        for (const json& note : list) {
            const std::string url = note.is_object() ? note.value("image", std::string()) : std::string();
            if (url.empty() || !ImageAllowed(url)) continue;
            const fs::path file = ImagePath(url);
            if (fs::exists(file, ec)) continue;
            fs::path temp = file;
            temp += L".tmp";
            if (net::Download(net::Widen(url), temp, nullptr, g_cancel, 4ull * 1024 * 1024) == 200) {
                fs::rename(temp, file, ec);
            } else {
                fs::remove(temp, ec);
            }
        }
    }
    std::lock_guard lock(g_mutex);
    RebuildNotesLocked();  // ahora con las imágenes que se bajaron
}

// Pasos 1 y 2: bajar latest.json y su firma, y comprobarla.
bool FetchManifest(json& manifest) {
    net::Response text;
    net::Response sig;
    if (!net::Request(L"GET", kManifestUrl, {}, L"", text, 256 * 1024) || text.status != 200 ||
        !net::Request(L"GET", kSignatureUrl, {}, L"", sig, 1024) || sig.status != 200) {
        SetState(State::Offline);
        return false;
    }
    if (!signature::Verify(text.body, sig.body)) {
        SetState(State::Failed, "update.bad_signature");
        return false;
    }
    manifest = json::parse(text.body, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("version")) {
        SetState(State::Failed, "update.failed", "latest.json");
        return false;
    }
    SaveCache(text.body);
    ApplyManifest(manifest);
    return true;
}

// Paso 3: bajar el .exe, comprobarlo y cambiarlo por el actual.
void Install(const json& manifest) {
    const std::string version = manifest.value("version", std::string());
    const std::string url = manifest.value("url", std::string());
    const std::string sha = manifest.value("sha256", std::string());
    if (url.rfind(kAllowedDownloadPrefix, 0) != 0 || sha.size() != 64) {
        SetState(State::Failed, "update.failed", "url");
        return;
    }
    const fs::path exe = ExePath();
    fs::path fresh = exe;
    fresh += L".new";
    fs::path old = exe;
    old += L".old";
    std::error_code ec;

    SetState(State::Downloading);
    const int status = net::Download(
        net::Widen(url), fresh,
        [](std::uint64_t done, std::uint64_t total) {
            std::lock_guard lock(g_mutex);
            g_info.progress = total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f;
        },
        g_cancel, kMaxExeSize);
    if (status != 200) {
        fs::remove(fresh, ec);
        SetState(g_cancel.load() ? State::Idle : State::Failed, "update.download_failed",
                 status > 0 ? "HTTP " + std::to_string(status) : std::string());
        return;
    }
    SetState(State::Installing);
    if (signature::Sha256File(fresh) != sha) {
        fs::remove(fresh, ec);
        SetState(State::Failed, "update.bad_signature", "sha256");
        return;
    }
    // Cambiar los archivos (con vuelta atrás si algo falla a la mitad).
    fs::remove(old, ec);
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(fresh, ec);
        SetState(State::Failed, "update.no_permission", exe.parent_path().string());
        return;
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);  // devolver el original
        fs::remove(fresh, ec);
        SetState(State::Failed, "update.no_permission", exe.parent_path().string());
        return;
    }
    // Abrir la versión nueva (le avisamos de dónde venimos) y cerrar esta.
    std::wstring command = L"\"" + exe.native() + L"\" --updated-from=" + net::Widen(kVersion);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        // Raro, pero el archivo ya está: la próxima vez que lo abras será la nueva.
        SetState(State::Failed, "update.restart_failed");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    {
        std::lock_guard lock(g_mutex);
        g_info.latestVersion = version;
    }
    SetState(State::Restarting);
}

void RunCheck(bool allowInstall) {
    SetState(State::Checking);
    json manifest;
    if (!FetchManifest(manifest)) {
        return;
    }
    DownloadImages(manifest);
    const std::string latest = manifest.value("version", std::string());
    if (!Newer(latest, kVersion)) {
        SetState(State::UpToDate);
        return;
    }
    if (allowInstall && kOfficial) {
        Install(manifest);
    } else {
        SetState(State::Available);
    }
}

// Borra el ".old" de una actualización anterior (el proceso viejo puede tardar
// un instante en cerrarse: se reintenta unos segundos).
void CleanupOld() {
    fs::path old = ExePath();
    old += L".old";
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        if (!fs::exists(old, ec) || fs::remove(old, ec)) {
            return;
        }
        Sleep(250);
    }
}

template <typename F>
void RunInBackground(F work) {
    if (g_busy.exchange(true)) {
        return;
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_cancel.store(false);
    g_worker = std::thread([work] {
        work();
        g_busy.store(false);
    });
}

}  // namespace

void Start(bool autoInstall, const wchar_t* commandLine) {
    g_autoInstall.store(autoInstall);
    if (const wchar_t* from = commandLine != nullptr ? std::wcsstr(commandLine, L"--updated-from=") : nullptr) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%ls", from + std::wcslen(L"--updated-from="));
        std::lock_guard lock(g_mutex);
        g_info.updatedFrom = text;
    }
    LoadCache();
    RunInBackground([] {
        CleanupOld();
        RunCheck(g_autoInstall.load());
    });
}

void CheckNow() {
    RunInBackground([] { RunCheck(g_autoInstall.load()); });
}

void InstallNow() {
    RunInBackground([] { RunCheck(true); });
}

void SetAutoInstall(bool enabled) {
    g_autoInstall.store(enabled);
}

Info Get() {
    std::lock_guard lock(g_mutex);
    Info info = g_info;
    // Las novedades en el idioma actual (si falta, en inglés).
    const char* lang = i18n::LanguageCode(i18n::CurrentLanguage());
    auto it = g_notes.find(lang);
    if (it == g_notes.end()) it = g_notes.find("en");
    if (it != g_notes.end()) info.notes = it->second;
    return info;
}

bool OfficialBuild() {
    return kOfficial;
}

void Shutdown() {
    g_cancel.store(true);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

}  // namespace luma::launcher::updater
