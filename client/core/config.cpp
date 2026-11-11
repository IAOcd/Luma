// =============================================================================
//  config.cpp - Configuration implementation
// =============================================================================
#include "core/config.h"
#include "core/tr.h"

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

// Converts UTF-8 text (how we store the names) to a Windows path.
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

// --- Reading and writing files -------------------------------------------------

// Reads a JSON. Returns false if it doesn't exist; "corrupt" = it exists but is damaged.
bool ReadJson(const fs::path& path, json& out, bool& corrupt) {
    corrupt = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    // parse(text, nullptr, false): the version that does NOT throw exceptions; if
    // the text isn't valid JSON it returns a "discarded" value.
    out = json::parse(buffer.str(), nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        corrupt = true;
        return false;
    }
    return true;
}

// Writes a JSON safely: to a .tmp and then replaces the real file.
bool WriteJsonAtomic(const fs::path& path, const json& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        // dump(4) = text indented with 4 spaces (readable if you open it).
        file << data.dump(4);
        if (!file) {
            return false;
        }
    }
    // MoveFileEx with REPLACE_EXISTING replaces the file in a single step.
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void BackupCorrupt(const fs::path& path) {
    std::error_code ec;
    fs::path backup = path;
    backup.replace_extension(L".corrupt.json");
    fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
    log::Warn("Damaged config: {}. Copy saved to {}.", paths::ToUtf8(path), paths::ToUtf8(backup));
}

// --- Profile <-> JSON (with the lock held) ---------------------------------------

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

// Applies a profile JSON. If something has an unexpected type, nlohmann throws
// an exception: we catch it and apply the defaults.
bool ApplyProfileJson(const json& data) {
    try {
        g_menuKey.store(data.value("menuKey", static_cast<int>('L')));
        g_unloadKey.store(data.value("unloadKey", static_cast<int>(VK_END)));
        theme::Load(data);
        render::hud::Load(data);
        modules::Manager::Load(data);
        return true;
    } catch (const std::exception& e) {
        log::Warn("Invalid values in the config ({}); using the defaults.", e.what());
        const json empty = json::object();
        g_menuKey.store('L');
        g_unloadKey.store(VK_END);
        theme::Load(empty);
        render::hud::Load(empty);
        modules::Manager::Load(empty);
        return false;
    }
}

// Loads the profile "name" into memory (with the lock held).
void LoadProfile(const std::string& name) {
    json data = json::object();
    bool corrupt = false;
    const fs::path path = ProfilePath(name);
    if (ReadJson(path, data, corrupt)) {
        if (!ApplyProfileJson(data)) {
            notify::Push(notify::Kind::Warning, Tr("The config had invalid values: defaults were used"));
        }
        log::Info("Profile loaded: {}", name);
    } else {
        if (corrupt) {
            BackupCorrupt(path);
            notify::Push(notify::Kind::Warning, Tr("Config was damaged: defaults were used (a copy was kept)"));
        }
        ApplyProfileJson(json::object());
        log::Info("Profile {} has no file: defaults.", name);
    }
    g_activeProfile = name;
}

bool WriteActiveProfileName() {
    json root;
    root["version"] = kConfigVersion;
    root["activeProfile"] = g_activeProfile;
    return WriteJsonAtomic(ConfigDir() / L"luma.json", root);
}

// Writes the active profile. Builds the JSON with the lock and writes without it.
bool WriteActiveProfile() {
    json data;
    std::string name;
    {
        std::lock_guard lock(StateMutex());
        data = BuildProfileJson();
        name = g_activeProfile;
    }
    if (!WriteJsonAtomic(ProfilePath(name), data)) {
        log::Error("Couldn't save profile {}.", name);
        return false;
    }
    return true;
}

}  // namespace

void LoadAtStartup() {
    if (ConfigDir().empty()) {
        log::Warn("No data folder: the config won't be saved.");
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
            // Name with an invalid type: we keep "default".
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
            entry.path().stem().extension().empty()) {  // skips *.corrupt.json
            names.push_back(paths::ToUtf8(entry.path().stem()));
        }
    }
    if (std::find(names.begin(), names.end(), g_activeProfile) == names.end()) {
        names.push_back(g_activeProfile);  // the active one even if it hasn't been saved yet
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
    // Save the current one before switching (with the lock: the JSON is built
    // quickly; writing the file is the only slow part, a few ms).
    WriteJsonAtomic(ProfilePath(g_activeProfile), BuildProfileJson());
    LoadProfile(name);
    WriteActiveProfileName();
    notify::Push(notify::Kind::Info, TrFormat("Profile: {}", name));
    return true;
}

bool CreateProfile(const std::string& name) {
    if (!IsValidProfileName(name)) {
        return false;
    }
    std::error_code ec;
    if (fs::exists(ProfilePath(name), ec)) {
        notify::Push(notify::Kind::Warning, Tr("A profile with that name already exists"));
        return false;
    }
    WriteJsonAtomic(ProfilePath(g_activeProfile), BuildProfileJson());
    // The new profile starts as a copy of the current one.
    if (!WriteJsonAtomic(ProfilePath(name), BuildProfileJson())) {
        notify::Push(notify::Kind::Error, Tr("Couldn't create the profile"));
        return false;
    }
    g_activeProfile = name;
    WriteActiveProfileName();
    notify::Push(notify::Kind::Success, TrFormat("Profile created: {}", name));
    return true;
}

bool DeleteProfile(const std::string& name) {
    if (name == g_activeProfile) {
        return false;
    }
    std::error_code ec;
    const bool removed = fs::remove(ProfilePath(name), ec);
    if (removed) {
        notify::Push(notify::Kind::Info, TrFormat("Profile deleted: {}", name));
    }
    return removed;
}

void ResetActiveProfile() {
    ApplyProfileJson(json::object());
    MarkDirty();
    notify::Push(notify::Kind::Info, Tr("Profile reset to defaults"));
}

}  // namespace luma::config
