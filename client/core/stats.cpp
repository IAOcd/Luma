// =============================================================================
//  stats.cpp - Implementación de las estadísticas (ver stats.h)
// =============================================================================
#include "core/stats.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/paths.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"

namespace luma::stats {

namespace {

namespace fs = std::filesystem;

constexpr ULONGLONG kSaveEveryMs = 60'000;   // estadísticas: cada minuto
constexpr ULONGLONG kNameEveryMs = 5'000;    // nombre: revisar cada 5 s

std::int64_t g_sessions = 0;
std::int64_t g_playSeconds = 0;     // lo que ya estaba guardado + lo sumado
ULONGLONG g_lastCount = 0;          // desde cuándo falta sumar tiempo
ULONGLONG g_lastSave = 0;
ULONGLONG g_lastNameCheck = 0;
std::string g_lastName;
bool g_begun = false;               // Begin() ya corrió (si Luma no arrancó, no se cuenta nada)

fs::path StatsPath() { return paths::DataDir() / L"stats.json"; }
fs::path ProfilePath() { return paths::DataDir() / L"profile.json"; }

// Escribir seguro: primero a ".tmp", después reemplazar (nunca queda a medias).
void WriteJson(const fs::path& path, const nlohmann::json& json) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += L".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        if (!file) {
            return;
        }
        file << json.dump(2);
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(temp, ec);
    }
}

nlohmann::json ReadJson(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return nlohmann::json::object();
    }
    nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
    return json.is_object() ? json : nlohmann::json::object();
}

// Suma el tiempo pasado desde la última vez y guarda.
void Save() {
    const ULONGLONG now = GetTickCount64();
    g_playSeconds += static_cast<std::int64_t>((now - g_lastCount) / 1000);
    g_lastCount = now - (now - g_lastCount) % 1000;  // los milisegundos sobrantes, para la próxima
    g_lastSave = now;
    nlohmann::json json = ReadJson(StatsPath());
    json["sessions"] = g_sessions;
    json["playSeconds"] = g_playSeconds;
    json["lastPlayed"] = static_cast<std::int64_t>(std::time(nullptr));  // segundos desde 1970 (hora UTC)
    WriteJson(StatsPath(), json);
}

// ¿Parece un gamertag? (1 a 32 caracteres visibles; si leímos basura, no)
bool LooksLikeName(const char* name) {
    const std::size_t length = std::strlen(name);
    if (length == 0 || length > 32) {
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

void CheckName() {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    if (player == 0) {
        return;
    }
    char name[64]{};
    if (!sdk::SafeReadString(player + sdk::offsets::kPlayerName, name, sizeof(name)) || !LooksLikeName(name) ||
        g_lastName == name) {
        return;
    }
    g_lastName = name;
    nlohmann::json json = ReadJson(ProfilePath());
    json["name"] = g_lastName;
    WriteJson(ProfilePath(), json);
    log::Info("Perfil: nombre del jugador guardado para el launcher.");
}

}  // namespace

void Begin() {
    const nlohmann::json json = ReadJson(StatsPath());
    g_sessions = json.value("sessions", static_cast<std::int64_t>(0)) + 1;
    g_playSeconds = json.value("playSeconds", static_cast<std::int64_t>(0));
    g_lastCount = GetTickCount64();
    g_begun = true;
    Save();
}

void Poll() {
    if (!g_begun) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastSave >= kSaveEveryMs) {
        Save();
    }
    if (now - g_lastNameCheck >= kNameEveryMs) {
        g_lastNameCheck = now;
        CheckName();
    }
}

void End() {
    if (!g_begun) {
        return;
    }
    Save();
}

}  // namespace luma::stats
