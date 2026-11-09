// =============================================================================
//  config.cpp - Implementación de la configuración
// =============================================================================
#include "core/config.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#include "core/logger.h"
#include "core/paths.h"
#include "core/state.h"
#include "modules/module_manager.h"
#include "render/notifications.h"
#include "render/hud.h"
#include "render/theme.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace luma::config {

std::atomic<int> g_menuKey{'L'};
std::atomic<int> g_unloadKey{VK_END};

namespace {

constexpr int kConfigVersion = 1;
constexpr ULONGLONG kSaveDelayMs = 1000;
const std::string kDefaultProfile = "default";

std::string g_activeProfile = kDefaultProfile;
std::atomic<bool> g_dirty{false};
std::atomic<ULONGLONG> g_dirtySince{0};

fs::path ConfigDir() {
    const fs::path data = paths::DataDir();
    return data.empty() ? fs::path{} : data / L"config";
}

fs::path ProfilesDir() {
    const fs::path dir = ConfigDir();
    return dir.empty() ? fs::path{} : dir / L"profiles";
}

// Convierte texto UTF-8 (como guardamos los nombres) a ruta de Windows.
fs::path FromUtf8(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return fs::path(wide);
}

fs::path ProfilePath(const std::string& name) {
    return ProfilesDir() / FromUtf8(name + ".json");
}

// --- Leer y escribir archivos --------------------------------------------------

// Lee un JSON. Devuelve false si no existe; "corrupt" = existe pero está dañado.
bool ReadJson(const fs::path& path, json& out, bool& corrupt) {
    corrupt = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    // parse(texto, nullptr, false): la versión que NO lanza excepciones; si el
    // texto no es JSON válido devuelve un valor "discarded".
    out = json::parse(buffer.str(), nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        corrupt = true;
        return false;
    }
    return true;
}

// Escribe un JSON de forma segura: a un .tmp y después lo reemplaza.
bool WriteJsonAtomic(const fs::path& path, const json& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        // dump(4) = texto con sangría de 4 espacios (legible si lo abres).
        file << data.dump(4);
        if (!file) {
            return false;
        }
    }
    // MoveFileEx con REPLACE_EXISTING reemplaza el archivo en un solo paso.
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void BackupCorrupt(const fs::path& path) {
    std::error_code ec;
    fs::path backup = path;
    backup.replace_extension(L".corrupto.json");
    fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
    log::Warn("Config dañada: {}. Copia guardada en {}.", paths::ToUtf8(path), paths::ToUtf8(backup));
}

// --- Perfil <-> JSON (con el candado tomado) -----------------------------------

json BuildProfileJson() {
    json data;
    data["version"] = kConfigVersion;
    data["menuKey"] = g_menuKey.load();
    data["unloadKey"] = g_unloadKey.load();
    theme::Save(data);
    render::hud::Save(data);
    modules::Manager::Save(data);
    return data;
}

// Aplica un JSON de perfil. Si algo tiene un tipo inesperado, nlohmann lanza
// una excepción: la atrapamos y aplicamos los valores de fábrica.
bool ApplyProfileJson(const json& data) {
    try {
        g_menuKey.store(data.value("menuKey", static_cast<int>('L')));
        g_unloadKey.store(data.value("unloadKey", static_cast<int>(VK_END)));
        theme::Load(data);
        render::hud::Load(data);
        modules::Manager::Load(data);
        return true;
    } catch (const std::exception& e) {
        log::Warn("Valores inválidos en la config ({}); se usan los de fábrica.", e.what());
        const json empty = json::object();
        g_menuKey.store('L');
        g_unloadKey.store(VK_END);
        theme::Load(empty);
        render::hud::Load(empty);
        modules::Manager::Load(empty);
        return false;
    }
}

// Carga el perfil "name" en memoria (con el candado tomado).
void LoadProfile(const std::string& name) {
    json data = json::object();
    bool corrupt = false;
    const fs::path path = ProfilePath(name);
    if (ReadJson(path, data, corrupt)) {
        if (!ApplyProfileJson(data)) {
            notify::Push(notify::Kind::Warning, "La config tenía valores inválidos: se usó la de fábrica");
        }
        log::Info("Perfil cargado: {}", name);
    } else {
        if (corrupt) {
            BackupCorrupt(path);
            notify::Push(notify::Kind::Warning, "Config dañada: se usó la de fábrica (hay una copia)");
        }
        ApplyProfileJson(json::object());
        log::Info("Perfil {} sin archivo: valores de fábrica.", name);
    }
    g_activeProfile = name;
}

bool WriteActiveProfileName() {
    json root;
    root["version"] = kConfigVersion;
    root["activeProfile"] = g_activeProfile;
    return WriteJsonAtomic(ConfigDir() / L"luma.json", root);
}

// Escribe el perfil activo. Arma el JSON con el candado y escribe sin él.
bool WriteActiveProfile() {
    json data;
    std::string name;
    {
        std::lock_guard lock(StateMutex());
        data = BuildProfileJson();
        name = g_activeProfile;
    }
    if (!WriteJsonAtomic(ProfilePath(name), data)) {
        log::Error("No se pudo guardar el perfil {}.", name);
        return false;
    }
    return true;
}

}  // namespace

void LoadAtStartup() {
    if (ConfigDir().empty()) {
        log::Warn("Sin carpeta de datos: la config no se guardará.");
        return;
    }
    std::string active = kDefaultProfile;
    json root;
    bool corrupt = false;
    if (ReadJson(ConfigDir() / L"luma.json", root, corrupt)) {
        try {
            const std::string saved = root.value("activeProfile", kDefaultProfile);
            if (IsValidProfileName(saved)) {
                active = saved;
            }
        } catch (const std::exception&) {
            // Nombre con tipo inválido: nos quedamos con "default".
        }
    }
    std::lock_guard lock(StateMutex());
    LoadProfile(active);
}

void MarkDirty() {
    g_dirtySince.store(GetTickCount64());
    g_dirty.store(true);
}

void SaveIfDirty() {
    if (!g_dirty.load() || GetTickCount64() - g_dirtySince.load() < kSaveDelayMs) {
        return;
    }
    g_dirty.store(false);
    WriteActiveProfile();
}

void SaveNow() {
    if (ConfigDir().empty()) {
        return;
    }
    g_dirty.store(false);
    WriteActiveProfile();
    std::lock_guard lock(StateMutex());
    WriteActiveProfileName();
}

const std::string& ActiveProfile() {
    return g_activeProfile;
}

std::vector<std::string> ListProfiles() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(ProfilesDir(), ec)) {
        if (entry.path().extension() == L".json" &&
            entry.path().stem().extension().empty()) {  // excluye *.corrupto.json
            names.push_back(paths::ToUtf8(entry.path().stem()));
        }
    }
    if (std::find(names.begin(), names.end(), g_activeProfile) == names.end()) {
        names.push_back(g_activeProfile);  // el activo aunque todavía no se haya guardado
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool IsValidProfileName(const std::string& name) {
    if (name.empty() || name.size() > 32 || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '-' || c == '_';
    });
}

bool SwitchProfile(const std::string& name) {
    if (!IsValidProfileName(name) || name == g_activeProfile) {
        return false;
    }
    // Guardar el actual antes de cambiar (con el candado: el JSON se arma
    // rápido; la escritura del archivo es lo único que tarda, unos ms).
    WriteJsonAtomic(ProfilePath(g_activeProfile), BuildProfileJson());
    LoadProfile(name);
    WriteActiveProfileName();
    notify::Push(notify::Kind::Info, std::format("Perfil: {}", name));
    return true;
}

bool CreateProfile(const std::string& name) {
    if (!IsValidProfileName(name)) {
        return false;
    }
    std::error_code ec;
    if (fs::exists(ProfilePath(name), ec)) {
        notify::Push(notify::Kind::Warning, "Ya existe un perfil con ese nombre");
        return false;
    }
    WriteJsonAtomic(ProfilePath(g_activeProfile), BuildProfileJson());
    // El nuevo perfil empieza como copia del actual.
    if (!WriteJsonAtomic(ProfilePath(name), BuildProfileJson())) {
        notify::Push(notify::Kind::Error, "No se pudo crear el perfil");
        return false;
    }
    g_activeProfile = name;
    WriteActiveProfileName();
    notify::Push(notify::Kind::Success, std::format("Perfil creado: {}", name));
    return true;
}

bool DeleteProfile(const std::string& name) {
    if (name == g_activeProfile) {
        return false;
    }
    std::error_code ec;
    const bool removed = fs::remove(ProfilePath(name), ec);
    if (removed) {
        notify::Push(notify::Kind::Info, std::format("Perfil borrado: {}", name));
    }
    return removed;
}

void ResetActiveProfile() {
    ApplyProfileJson(json::object());
    MarkDirty();
    notify::Push(notify::Kind::Info, "Perfil restablecido a los valores de fábrica");
}

}  // namespace luma::config
