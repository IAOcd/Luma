// =============================================================================
//  waypoints.cpp - Waypoints (see waypoints.h)
// =============================================================================
#include "modules/world/waypoints.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/logger.h"
#include "core/paths.h"
#include "core/tr.h"
#include "sdk/network.h"
#include "sdk/players.h"

namespace luma::modules::world::waypoints {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::vector<Waypoint> g_list;
std::string g_key;        // which world's list is loaded ("" = none)
std::string g_label;      // its name, for the menu
std::string g_candidate;  // what the last check saw (it must repeat to switch)
std::string g_candidateLabel;
int g_dimension = -1;
double g_nextCheck = 0.0;
double g_saveAt = -1.0;  // when to save (-1 = nothing pending)
double g_holdUntil = 0.0;  // (after a new world: don't decide which one before this)
double g_now = 0.0;

// The palette for new waypoints (one after another).
constexpr float kPalette[][3] = {{1.00f, 0.78f, 0.30f}, {0.36f, 0.78f, 1.00f}, {0.45f, 0.90f, 0.45f}, {1.00f, 0.42f, 0.42f},
                                 {0.80f, 0.50f, 1.00f}, {1.00f, 0.60f, 0.20f}, {0.30f, 0.95f, 0.85f}, {1.00f, 0.55f, 0.85f}};

// A file name from any text: letters, digits, "-" and "." stay; the rest
// becomes "_". Plus a short number made from the whole text (so two worlds
// whose names only differ in accents don't share a file).
std::string FileName(const std::string& key) {
    std::string out;
    for (const char ch : key) {
        const auto c = static_cast<unsigned char>(ch);
        out += (std::isalnum(c) != 0 && c < 128) || c == '-' || c == '.' ? static_cast<char>(c) : '_';
        if (out.size() >= 60) break;
    }
    std::uint32_t h = 2166136261u;  // FNV-1a
    for (const char ch : key) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    char tail[16];
    std::snprintf(tail, sizeof(tail), "_%08x.json", h);
    return out + tail;
}

fs::path FilePath(const std::string& key) {
    const fs::path dir = paths::DataDir();
    if (dir.empty()) return {};
    return dir / L"waypoints" / FileName(key);
}

void Save() {
    g_saveAt = -1.0;
    if (g_key.empty()) return;
    const fs::path path = FilePath(g_key);
    if (path.empty()) return;
    json root;
    root["world"] = g_label;
    json list = json::array();
    for (const Waypoint& w : g_list) {
        list.push_back({{"name", std::string(w.name.data())},
                        {"x", w.x},
                        {"y", w.y},
                        {"z", w.z},
                        {"dimension", w.dimension},
                        {"color", {w.color[0], w.color[1], w.color[2]}},
                        {"visible", w.visible},
                        {"inWorld", w.inWorld},
                        {"death", w.death}});
    }
    root["waypoints"] = std::move(list);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // To a .tmp first and then replace: a crash halfway never leaves a broken file.
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            log::Warn("Waypoints: couldn't write {}", paths::ToUtf8(temp));
            return;
        }
        file << root.dump(2);
    }
    fs::rename(temp, path, ec);
    if (ec) log::Warn("Waypoints: couldn't save {} ({})", paths::ToUtf8(path), ec.message());
}

void Load(const std::string& key) {
    g_list.clear();
    const fs::path path = FilePath(key);
    std::ifstream file(path, std::ios::binary);
    if (!file) return;  // no file yet: no waypoints in this world
    try {
        const json root = json::parse(file);
        for (const json& j : root.value("waypoints", json::array())) {
            Waypoint w;
            const std::string name = j.value("name", std::string("?"));
            strncpy_s(w.name.data(), w.name.size(), name.c_str(), _TRUNCATE);
            w.x = j.value("x", 0);
            w.y = j.value("y", 64);
            w.z = j.value("z", 0);
            w.dimension = std::clamp(j.value("dimension", 0), 0, 2);
            if (j.contains("color") && j["color"].is_array() && j["color"].size() == 3) {
                for (int k = 0; k < 3; ++k) w.color[k] = std::clamp(j["color"][k].get<float>(), 0.0f, 1.0f);
            }
            w.visible = j.value("visible", true);
            w.inWorld = j.value("inWorld", true);
            w.death = j.value("death", false);
            g_list.push_back(w);
        }
    } catch (const std::exception& e) {
        log::Warn("Waypoints: {} is damaged ({}); starting empty.", paths::ToUtf8(path), e.what());
        g_list.clear();
    }
}

}  // namespace

void Update(double now, int dimension) {
    g_dimension = dimension;
    g_now = now;
    if (g_saveAt >= 0.0 && now >= g_saveAt) Save();
    if (now < g_nextCheck || now < g_holdUntil) return;
    g_nextCheck = now + 2.0;
    // Where are we? A server by its address; a local world by its name.
    std::string key, label;
    if (sdk::network::OnServer()) {
        sdk::players::Server server;
        std::string name;
        if (sdk::players::ReadServer(server) && !server.address.empty()) {
            label = server.address + ":" + std::to_string(server.port);
            key = "server_" + label;
        } else if (sdk::players::WorldName(name) && !name.empty()) {
            // (Some servers and Realms don't let Luma read their address: then
            // the world's name the server gives, kept apart from local worlds.)
            label = name;
            key = "servername_" + name;
        }
    } else {
        std::string name;
        if (sdk::players::WorldName(name) && !name.empty()) {
            label = name;
            key = "world_" + name;
        }
    }
    // Change only when the same answer comes twice in a row (when you join a
    // server, the first seconds can look like a local world).
    if (key != g_candidate) {
        g_candidate = key;
        g_candidateLabel = label;
        return;
    }
    if (key == g_key || key.empty()) return;  // (empty: in a menu or loading)
    if (g_saveAt >= 0.0) Save();
    g_key = key;
    g_label = g_candidateLabel;
    Load(key);
    log::Info("Waypoints: {} ({} saved).", g_label, g_list.size());
}

void ForgetWorld(double now) {
    if (g_saveAt >= 0.0) Save();
    if (!g_key.empty()) log::Info("Waypoints: left {}.", g_label);
    g_key.clear();
    g_label.clear();
    g_candidate.clear();
    g_list.clear();
    g_holdUntil = now + 6.0;
}

bool HaveWorld() {
    return !g_key.empty();
}

const std::string& WorldLabel() {
    return g_label;
}

int Dimension() {
    return g_dimension;
}

std::string StorageName() {
    if (g_key.empty()) return {};
    std::string name = FileName(g_key);
    return name.substr(0, name.size() - 5);  // (without ".json")
}

std::vector<Waypoint>& List() {
    return g_list;
}

std::size_t Add(int x, int y, int z) {
    Waypoint w;
    // "Point N": the first number not used yet.
    for (int n = 1; n < 1000; ++n) {
        char name[32];
        std::snprintf(name, sizeof(name), "%s %d", Tr("Point"), n);
        const bool used = std::any_of(g_list.begin(), g_list.end(), [&](const Waypoint& o) { return std::strcmp(o.name.data(), name) == 0; });
        if (!used) {
            strncpy_s(w.name.data(), w.name.size(), name, _TRUNCATE);
            break;
        }
    }
    w.x = x;
    w.y = y;
    w.z = z;
    w.dimension = std::max(0, g_dimension);
    const auto& c = kPalette[g_list.size() % (sizeof(kPalette) / sizeof(kPalette[0]))];
    std::copy(c, c + 3, w.color);
    g_list.push_back(w);
    MarkDirty();
    return g_list.size() - 1;
}

void Remove(std::size_t index) {
    if (index >= g_list.size()) return;
    g_list.erase(g_list.begin() + static_cast<std::ptrdiff_t>(index));
    MarkDirty();
}

void SetDeathPoint(int x, int y, int z) {
    g_list.erase(std::remove_if(g_list.begin(), g_list.end(), [](const Waypoint& w) { return w.death; }), g_list.end());
    Waypoint w;
    strncpy_s(w.name.data(), w.name.size(), Tr("Death"), _TRUNCATE);
    w.x = x;
    w.y = y;
    w.z = z;
    w.dimension = std::max(0, g_dimension);
    w.color[0] = 0.85f;
    w.color[1] = 0.16f;
    w.color[2] = 0.16f;
    w.death = true;
    g_list.push_back(w);
    MarkDirty();
}

void MarkDirty() {
    // (a second later: typing a name doesn't write the file at every letter)
    g_saveAt = g_now + 1.0;
}

void Flush() {
    if (g_saveAt >= 0.0) Save();
}

}  // namespace luma::modules::world::waypoints
