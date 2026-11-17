// =============================================================================
//  flow_presets.h - YOUR Lyric Flow styles (shared by Luma.dll and the launcher)
// =============================================================================
//  The six ready-made styles live in flow_styles.h. These are yours: you save
//  how your lyrics look (all of Lyric Flow's settings: colors, effects, size,
//  where they are...) with a name, and bring it back with one click, in the
//  game or in the launcher. They're kept in one file both read:
//      %APPDATA%\Luma\config\flow_styles.json
//
//  YOUR CONFIG IS NEVER LOST: when you apply a style (ready-made or yours)
//  and what you had isn't saved anywhere, it's saved first, automatically, as
//  "Your previous style" (one slot: each time it keeps the newest).
//
//  A style is Lyric Flow's object from the profile (modules > now_playing >
//  settings > lyricFlow) minus what isn't "how it looks": whether it's on,
//  and the key that pins it.
// =============================================================================
#pragma once

#include <windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace luma::flowpresets {

struct Preset {
    std::string name;
    nlohmann::json look = nlohmann::json::object();
    bool autoSaved = false;  // "Your previous style" (the automatic copy)
};

// The file, inside Luma's data folder (%APPDATA%\Luma).
inline std::filesystem::path FileIn(const std::filesystem::path& dataDir) {
    return dataDir.empty() ? std::filesystem::path{} : dataDir / L"config" / L"flow_styles.json";
}

inline bool IsStyleKey(const std::string& key) {
    return key != "on" && key != "pinKey";
}

// The part of Lyric Flow's settings that a style keeps.
inline nlohmann::json LookOf(const nlohmann::json& flow) {
    nlohmann::json look = nlohmann::json::object();
    if (!flow.is_object()) return look;
    for (auto it = flow.begin(); it != flow.end(); ++it) {
        if (IsStyleKey(it.key())) look[it.key()] = it.value();
    }
    return look;
}

// Puts a style on Lyric Flow's settings (what it doesn't have stays).
inline void ApplyTo(nlohmann::json& flow, const nlohmann::json& look) {
    if (!flow.is_object()) flow = nlohmann::json::object();
    if (!look.is_object()) return;
    for (auto it = look.begin(); it != look.end(); ++it) {
        if (IsStyleKey(it.key())) flow[it.key()] = it.value();
    }
}

// Two values the same (numbers: almost, since they go through floats).
inline bool SameValue(const nlohmann::json& a, const nlohmann::json& b) {
    if (a.is_number() && b.is_number()) return std::fabs(a.get<double>() - b.get<double>()) < 0.001;
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!SameValue(a[i], b[i])) return false;
        }
        return true;
    }
    return a == b;
}

// Is "look" exactly the saved style "preset"? (every setting the style has)
inline bool SameLook(const nlohmann::json& preset, const nlohmann::json& look) {
    if (!preset.is_object() || !look.is_object() || preset.empty()) return false;
    for (auto it = preset.begin(); it != preset.end(); ++it) {
        if (!IsStyleKey(it.key())) continue;
        const auto other = look.find(it.key());
        if (other == look.end() || !SameValue(it.value(), *other)) return false;
    }
    return true;
}

// Which of your styles "look" is (-1 = none).
inline int Find(const std::vector<Preset>& list, const nlohmann::json& look) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (SameLook(list[i].look, look)) return static_cast<int>(i);
    }
    return -1;
}

inline std::vector<Preset> Load(const std::filesystem::path& file) {
    std::vector<Preset> list;
    std::ifstream in(file, std::ios::binary);
    if (!in) return list;
    const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (!json.is_object()) return list;
    const auto styles = json.find("styles");
    if (styles == json.end() || !styles->is_array()) return list;
    for (const auto& s : *styles) {
        if (!s.is_object()) continue;
        Preset p;
        const auto name = s.find("name");
        if (name != s.end() && name->is_string()) p.name = name->get<std::string>();
        const auto look = s.find("lyricFlow");
        if (look != s.end() && look->is_object()) p.look = LookOf(*look);
        const auto autoSaved = s.find("auto");
        p.autoSaved = autoSaved != s.end() && autoSaved->is_boolean() && autoSaved->get<bool>();
        if (!p.name.empty() && !p.look.empty()) list.push_back(std::move(p));
    }
    return list;
}

// Written safely: to a .tmp, then it replaces the real file.
inline bool Save(const std::filesystem::path& file, const std::vector<Preset>& list) {
    if (file.empty()) return false;
    nlohmann::json styles = nlohmann::json::array();
    for (const Preset& p : list) {
        nlohmann::json s = {{"name", p.name}, {"lyricFlow", p.look}};
        if (p.autoSaved) s["auto"] = true;
        styles.push_back(std::move(s));
    }
    nlohmann::json json;
    json["version"] = 1;
    json["styles"] = std::move(styles);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path temp = std::filesystem::path(file).concat(L".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << json.dump(2);
        if (!out) return false;
    }
    return MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// "My style 1", "My style 2"... (the first number not taken).
inline std::string NextName(const std::vector<Preset>& list, const std::string& base) {
    for (int n = 1;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        bool taken = false;
        for (const Preset& p : list) taken = taken || p.name == name;
        if (!taken) return name;
    }
}

// Before applying a style: if what you have now isn't saved anywhere (not a
// ready-made style, "isReady", and none of yours), keep it in the automatic
// slot. Returns true if the list changed (then save it).
inline bool BackupIfUnsaved(std::vector<Preset>& list, const nlohmann::json& look, bool isReady, const std::string& backupName) {
    if (isReady || look.empty() || Find(list, look) >= 0) return false;
    for (Preset& p : list) {
        if (p.autoSaved) {
            p.look = look;
            p.name = backupName;
            return true;
        }
    }
    list.push_back(Preset{backupName, look, true});
    return true;
}

}  // namespace luma::flowpresets
