// =============================================================================
//  stats.cpp - Stats implementation (see stats.h)
// =============================================================================
#include "core/stats.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/paths.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"
#include "sdk/skin.h"
#include "render/skin_heads.h"

namespace luma::stats {

namespace {

namespace fs = std::filesystem;

constexpr ULONGLONG kSaveEveryMs = 60'000;   // stats: every minute
constexpr ULONGLONG kNameEveryMs = 5'000;    // name: check every 5 s
constexpr ULONGLONG kSkinEveryMs = 10'000;   // skin: check every 10 s

std::int64_t g_sessions = 0;
std::int64_t g_playSeconds = 0;     // what was already saved + what was added
ULONGLONG g_lastCount = 0;          // since when time still needs to be added
ULONGLONG g_lastSave = 0;
ULONGLONG g_lastNameCheck = 0;
std::string g_lastName;
bool g_begun = false;
ULONGLONG g_lastSkinCheck = 0;
std::uint64_t g_lastSkinHash = 0;               // Begin() already ran (if Luma didn't start, nothing is counted)

fs::path StatsPath() { return paths::DataDir() / L"stats.json"; }
fs::path ProfilePath() { return paths::DataDir() / L"profile.json"; }

// Safe write: first to ".tmp", then replace (it's never left half written).
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

// Adds the time elapsed since last time and saves.
void Save() {
    const ULONGLONG now = GetTickCount64();
    g_playSeconds += static_cast<std::int64_t>((now - g_lastCount) / 1000);
    g_lastCount = now - (now - g_lastCount) % 1000;  // the leftover milliseconds, for next time
    g_lastSave = now;
    nlohmann::json json = ReadJson(StatsPath());
    json["sessions"] = g_sessions;
    json["playSeconds"] = g_playSeconds;
    json["lastPlayed"] = static_cast<std::int64_t>(std::time(nullptr));  // seconds since 1970 (UTC)
    WriteJson(StatsPath(), json);
}

// Does it look like a gamertag? (1 to 32 visible characters; if we read garbage, no)
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
    log::Info("Profile: player name saved for the launcher.");
}

// --- The skin (for the launcher) -------------------------------------------------
// Saves the RGBA pixels as a PNG with WIC (Windows' image encoder).
bool WritePng(const fs::path& path, const sdk::skin::Image& image) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // (if it already existed, nothing happens)
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    // Windows' PNG encoder wants the pixels as BGRA (blue first):
    // we swap them (the game's skin comes as RGBA).
    std::vector<std::uint8_t> bgra(image.rgba);
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        std::swap(bgra[i], bgra[i + 2]);
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
              IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

void CheckSkin() {
    sdk::skin::Image image;
    if (!sdk::skin::ReadLocal(image)) {
        return;
    }
    // Did it change? (a "fingerprint" of the pixels: if it's the same, nothing is written)
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::uint8_t b : image.rgba) hash = (hash ^ b) * 0x100000001B3ull;
    if (hash == g_lastSkinHash) {
        return;
    }
    g_lastSkinHash = hash;
    render::skinheads::SetOwn(image.rgba.data(), image.width, image.height);  // the radial menu's face
    std::error_code ec;
    fs::create_directories(paths::DataDir(), ec);
    fs::path temp = paths::DataDir() / L"skin.png.tmp";
    if (!WritePng(temp, image) || !MoveFileExW(temp.c_str(), (paths::DataDir() / L"skin.png").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(temp, ec);
        static bool warned = false;
        if (!warned) {
            warned = true;
            log::Warn("Skin: read ({}x{}) but the PNG couldn't be saved.", image.width, image.height);
        }
        return;
    }
    nlohmann::json json;
    json["width"] = image.width;
    json["height"] = image.height;
    json["id"] = image.id;
    WriteJson(paths::DataDir() / L"skin.json", json);
    log::Info("Skin: saved for the launcher ({}x{}).", image.width, image.height);
}

// The game folder (so the launcher can find its pixel font).
void SaveGameDir() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    nlohmann::json json = ReadJson(ProfilePath());
    json["gameDir"] = paths::ToUtf8(fs::path(std::wstring(exe, length)).parent_path());
    WriteJson(ProfilePath(), json);
}

}  // namespace

void Begin() {
    const nlohmann::json json = ReadJson(StatsPath());
    g_sessions = json.value("sessions", static_cast<std::int64_t>(0)) + 1;
    g_playSeconds = json.value("playSeconds", static_cast<std::int64_t>(0));
    g_lastCount = GetTickCount64();
    g_begun = true;
    Save();
    SaveGameDir();
    // Your face for the menu from the last saved skin (before joining a world).
    render::skinheads::LoadOwnFromFile((paths::DataDir() / L"skin.png").native());
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
    if (now - g_lastSkinCheck >= kSkinEveryMs) {
        g_lastSkinCheck = now;
        CheckSkin();
    }
}

void End() {
    if (!g_begun) {
        return;
    }
    Save();
}

}  // namespace luma::stats
