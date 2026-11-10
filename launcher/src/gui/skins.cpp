// =============================================================================
//  skins.cpp - La biblioteca de skins (ver skins.h)
// =============================================================================
#include "gui/skins.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>  // IFileOpenDialog: el diálogo "Abrir" de Windows

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "gui/skin3d.h"
#include "gui/textures.h"
#include "signature.h"

namespace luma::launcher::gui::skins {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::vector<Skin> g_skins;

fs::path Dir() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma" / L"skins";
    }
    CoTaskMemFree(raw);
    return path;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void Save() {
    std::error_code ec;
    fs::create_directories(Dir(), ec);
    json list = json::array();
    for (const Skin& s : g_skins) {
        list.push_back({{"id", s.id},
                        {"name", s.name},
                        {"file", fs::path(s.file).filename().string()},
                        {"slim", s.slim},
                        {"legacy", s.legacy},
                        {"favorite", s.favorite},
                        {"fromGame", s.fromGame},
                        {"added", s.added}});
    }
    const fs::path temp = Dir() / L"library.json.tmp";
    std::ofstream(temp, std::ios::trunc) << json{{"skins", list}}.dump(2);
    MoveFileExW(temp.c_str(), (Dir() / L"library.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ¿Es una skin de Minecraft? Tamaños válidos: 64x64, 64x32 y sus versiones HD.
bool Inspect(const std::wstring& png, bool& slim, bool& legacy) {
    std::vector<std::uint8_t> rgba;
    unsigned w = 0;
    unsigned h = 0;
    if (!textures::DecodeFile(png, rgba, w, h) || w < 64 || w % 64 != 0 || (h != w && h * 2 != w) || w > 1024) {
        return false;
    }
    legacy = h * 2 == w;
    slim = !legacy && skin3d::DetectSlim(rgba.data(), static_cast<int>(w), static_cast<int>(h));
    return true;
}

Skin* Find(const std::string& id) {
    for (Skin& s : g_skins) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

// Copia un PNG a la biblioteca (si no estaba). Devuelve el id ("" si no es una skin).
std::string Add(const std::wstring& source, const std::string& name, bool fromGame) {
    bool slim = false;
    bool legacy = false;
    if (!Inspect(source, slim, legacy)) return {};
    const std::string id = signature::Sha256File(source);
    if (id.empty()) return {};
    if (Find(id) != nullptr) return id;  // ya estaba
    std::error_code ec;
    fs::create_directories(Dir(), ec);
    const fs::path target = Dir() / (id.substr(0, 16) + ".png");
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    if (ec) return {};
    Skin skin;
    skin.id = id;
    skin.name = name;
    skin.file = target.native();
    skin.slim = slim;
    skin.legacy = legacy;
    skin.fromGame = fromGame;
    skin.added = static_cast<std::int64_t>(std::time(nullptr));
    g_skins.insert(g_skins.begin(), skin);
    Save();
    return id;
}

}  // namespace

void Load() {
    g_skins.clear();
    std::ifstream file(Dir() / L"library.json");
    if (!file) return;
    const json root = json::parse(file, nullptr, false);
    if (!root.is_object() || !root.contains("skins") || !root["skins"].is_array()) return;
    for (const json& j : root["skins"]) {
        if (!j.is_object()) continue;
        Skin s;
        s.id = j.value("id", std::string());
        s.name = j.value("name", std::string());
        s.file = (Dir() / fs::path(j.value("file", std::string()))).native();
        s.slim = j.value("slim", false);
        s.legacy = j.value("legacy", false);
        s.favorite = j.value("favorite", false);
        s.fromGame = j.value("fromGame", false);
        s.added = j.value("added", static_cast<std::int64_t>(0));
        std::error_code ec;
        if (!s.id.empty() && fs::exists(s.file, ec)) g_skins.push_back(std::move(s));
    }
}

const std::vector<Skin>& List() {
    return g_skins;
}

std::string SyncFromGame(const std::wstring& png) {
    // Nombre: "Skin del juego · 10.11.2026" (se puede cambiar más adelante).
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(date, sizeof(date), "%d.%m.%Y", &tm);
    return Add(png, std::string("Minecraft · ") + date, true);
}

std::string ImportWithDialog(void* owner, std::string& errorKey) {
    errorKey.clear();
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        errorKey = "skins.import_error";
        return {};
    }
    const COMDLG_FILTERSPEC filter[] = {{L"PNG", L"*.png"}};
    dialog->SetFileTypes(1, filter);
    std::string id;
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner)))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            const fs::path file(path);
            id = Add(file.native(), ToUtf8(file.stem().native()), false);
            if (id.empty()) errorKey = "skins.import_error";
        }
        if (path) CoTaskMemFree(path);
        if (item) item->Release();
    }
    dialog->Release();
    return id;
}

std::string Active() {
    // Se lee del archivo cada vez que se pide desde la interfaz... pero barato:
    // se guarda en memoria y se vuelve a leer solo si el archivo cambió (el Closet
    // del juego también lo escribe).
    static fs::file_time_type lastWrite{};
    static std::string active;
    std::error_code ec;
    const fs::path file = Dir() / L"active.json";
    const auto written = fs::last_write_time(file, ec);
    if (ec) return {};
    if (written != lastWrite) {
        lastWrite = written;
        std::ifstream in(file);
        const json j = in ? json::parse(in, nullptr, false) : json();
        active = j.is_object() ? j.value("id", std::string()) : std::string();
    }
    return active;
}

void SetActive(const std::string& id) {
    std::error_code ec;
    fs::create_directories(Dir(), ec);
    std::ofstream(Dir() / L"active.json", std::ios::trunc) << json{{"id", id}}.dump(2);
}

void SetFavorite(const std::string& id, bool favorite) {
    if (Skin* s = Find(id)) {
        s->favorite = favorite;
        Save();
    }
}

void Remove(const std::string& id) {
    auto it = std::find_if(g_skins.begin(), g_skins.end(), [&](const Skin& s) { return s.id == id; });
    if (it == g_skins.end()) return;
    std::error_code ec;
    fs::remove(it->file, ec);
    g_skins.erase(it);
    Save();
}

std::wstring Folder() {
    std::error_code ec;
    fs::create_directories(Dir(), ec);
    return Dir().native();
}

}  // namespace luma::launcher::gui::skins
