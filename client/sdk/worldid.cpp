// =============================================================================
//  worldid.cpp - Which world is this, exactly? (see worldid.h)
// =============================================================================
#include "sdk/worldid.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>
#include <vector>

#include "core/logger.h"
#include "core/paths.h"
#include "sdk/memory.h"
#include "sdk/network.h"
#include "sdk/players.h"

namespace luma::sdk::worldid {

namespace {

namespace fs = std::filesystem;

std::set<std::string> g_folders;        // the names of your world folders
std::vector<fs::path> g_folderPaths;    // (and where they are)
bool g_listed = false;
int g_folderOffset = -2;   // where the folder name is in the Level object (-2 = not searched, -1 = gave up)
int g_seedOffset = -2;     // where the seed is in the LevelData (-2 = not searched, -1 = gave up)
int g_folderTries = 0, g_seedTries = 0;
std::string g_folder;
std::int64_t g_seed = 0;
bool g_haveSeed = false;
std::uintptr_t g_lastLevel = 0;
bool g_seedInLevel = false;  // (the seed is in the Level, not the LevelData)
bool g_seed32 = false;       // (kept as 4 bytes)
int g_seedPtr = -1;          // (>= 0: the seed is in the object pointed to from here)

// Every folder in every "minecraftWorlds" of every account (Bedrock GDK:
// %APPDATA%\Minecraft Bedrock\Users\<id>\games\com.mojang\minecraftWorlds).
void ListFolders() {
    g_listed = true;
    g_folders.clear();
    g_folderPaths.clear();
    const fs::path appdata = paths::DataDir().parent_path();  // (%APPDATA%; DataDir is %APPDATA%\Luma)
    std::error_code ec;
    const fs::path users = appdata / L"Minecraft Bedrock" / L"Users";
    for (const auto& user : fs::directory_iterator(users, ec)) {
        const fs::path worlds = user.path() / L"games" / L"com.mojang" / L"minecraftWorlds";
        for (const auto& w : fs::directory_iterator(worlds, ec)) {
            if (!w.is_directory(ec)) continue;
            const std::string name = paths::ToUtf8(w.path().filename());
            if (name.size() >= 8 && name.size() <= 24) {
                g_folders.insert(name);
                g_folderPaths.push_back(w.path());
            }
        }
    }
    log::Info("World id: {} world folders found.", g_folders.size());
}

// The seed written in a world's level.dat (it's NBT: a "long" tag named RandomSeed).
bool SeedFromFile(const std::string& folder, std::int64_t& out) {
    for (const fs::path& p : g_folderPaths) {
        if (paths::ToUtf8(p.filename()) != folder) continue;
        std::ifstream file(p / L"level.dat", std::ios::binary);
        if (!file) return false;
        std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        static const char kTag[] = "\x04\x0A\x00RandomSeed";  // type 4 (long), name length 10, name
        for (std::size_t i = 0; i + sizeof(kTag) - 1 + 8 <= data.size(); ++i) {
            if (std::memcmp(data.data() + i, kTag, sizeof(kTag) - 1) == 0) {
                std::memcpy(&out, data.data() + i + sizeof(kTag) - 1, 8);
                return true;
            }
        }
        return false;
    }
    return false;
}

// THE GAME'S OWN SEED GETTER, read without running it. BlockSource's
// function n.o 45 (+0x168, 1.26.63) takes the Level (BlockSource+0x20) and
// calls the Level's function at +0x438: getLevelSeed64. Luma reads that
// function's first instructions and, if they're only simple reads ("take
// field X", "from there field Y"... then return), does the same reads
// itself. Anything else: not used (and its bytes go to the log).
constexpr std::uintptr_t kLevelSeedSlot = 0x438;

using SeedGetterFn = std::int64_t*(__fastcall*)(void* level, std::int64_t* out);
bool CallGetter(std::uintptr_t fn, std::uintptr_t level, std::int64_t& out) {
    std::int64_t value = 0;
    __try {
        reinterpret_cast<SeedGetterFn>(fn)(reinterpret_cast<void*>(level), &value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    out = value;
    return true;
}

bool SeedFromGetter(std::uintptr_t level, std::int64_t& out, bool logBytes) {
    std::uintptr_t vt = 0, fn = 0;
    if (!SafeRead(level, vt) || !SafeRead(vt + kLevelSeedSlot, fn) || !LooksLikePointer(fn)) return false;
    std::uint8_t code[32]{};
    if (!SafeCopy(fn, code, sizeof(code))) return false;
    // The getter of 1.26.63 (analyzed from the game's file): it asks the
    // Level for its property list (its function +0x560) and looks the seed
    // up by name, as a 64-bit number. It only reads. If its first bytes are
    // exactly these, it's called (protected: an error just means "no seed").
    static const std::uint8_t kKnownGetter[] = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD6, 0x48, 0x8B,
                                                0x01, 0x48, 0x8B, 0x80, 0x60, 0x05, 0x00, 0x00, 0xFF, 0x15};
    if (std::memcmp(code, kKnownGetter, sizeof(kKnownGetter)) == 0) {
        return CallGetter(fn, level, out);
    }
    std::uintptr_t rax = 0, rcx = level;
    bool raxSet = false;
    int i = 0;
    for (int steps = 0; steps < 6; ++steps) {
        if (code[i] == 0xC3) {  // ret
            if (!raxSet) break;
            out = static_cast<std::int64_t>(rax);
            return true;
        }
        // mov r64, [r64 + disp]  (48 8B modrm disp8/disp32)
        if (code[i] != 0x48 || code[i + 1] != 0x8B) break;
        const std::uint8_t modrm = code[i + 2];
        const int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
        if ((mod != 1 && mod != 2) || (reg != 0 && reg != 1) || (rm != 0 && rm != 1)) break;
        std::int32_t disp = 0;
        int len = 3;
        if (mod == 1) {
            disp = static_cast<std::int8_t>(code[i + 3]);
            len = 4;
        } else {
            std::memcpy(&disp, code + i + 3, 4);
            len = 7;
        }
        const std::uintptr_t src = (rm == 0 ? rax : rcx) + static_cast<std::intptr_t>(disp);
        std::uintptr_t v = 0;
        if (!SafeRead(src, v)) return false;
        if (reg == 0) {
            rax = v;
            raxSet = true;
        } else {
            rcx = v;
        }
        i += len;
        if (i > 24) break;
    }
    if (logBytes) {
        char hex[100];
        int n = 0;
        for (int k = 0; k < 24; ++k) n += std::snprintf(hex + n, sizeof(hex) - static_cast<std::size_t>(n), "%02X ", code[k]);
        log::Info("World id: the seed getter isn't a simple read ({}).", hex);
    }
    return false;
}

}  // namespace

void Update() {
    const std::uintptr_t level = sdk::players::Level();
    if (level != g_lastLevel) {
        g_lastLevel = level;
        g_folder.clear();
        g_haveSeed = false;
    }
    if (level == 0) return;
    const bool online = sdk::network::OnServer();
    // 1. The folder (local worlds only).
    if (!online) {
        if (!g_listed) ListFolders();
        char text[40];
        if (g_folderOffset == -2) {
            for (int off = 0; off < 0x4000; off += 8) {
                if (SafeReadString(level + static_cast<std::uintptr_t>(off), text, sizeof(text)) && g_folders.count(text) != 0) {
                    g_folderOffset = off;
                    log::Info("World id: the world's folder is at Level+0x{:X} (\"{}\").", off, text);
                    break;
                }
            }
            if (g_folderOffset == -2 && ++g_folderTries >= 5) {
                // (a world created just now isn't in the list yet: look again once)
                if (g_folderTries == 5) ListFolders();
                if (g_folderTries >= 10) {
                    g_folderOffset = -1;
                    log::Warn("World id: the world's folder wasn't found in memory.");
                }
            }
        }
        if (g_folderOffset >= 0 && g_folder.empty() && SafeReadString(level + static_cast<std::uintptr_t>(g_folderOffset), text, sizeof(text))) {
            g_folder = text;
            if (g_folders.count(g_folder) == 0) ListFolders();
        }
    }
    // 2. The seed: learned once from a local world's level.dat, then read from memory.
    const std::uintptr_t data = sdk::players::LevelData();
    if (data == 0) return;
    if (g_seedOffset == -2 && !g_folder.empty()) {
        std::int64_t fileSeed = 0;
        if (SeedFromFile(g_folder, fileSeed) && fileSeed != 0) {
            // In the LevelData, then in the Level; as 8 bytes, or as 4 (the
            // low half: older worlds kept a 32-bit seed).
            const std::uintptr_t bases[2] = {data, level};
            for (int b = 0; b < 2 && g_seedOffset == -2; ++b) {
                for (int off = 0; off < 0x4000; off += 4) {
                    std::int64_t v = 0;
                    std::int32_t v32 = 0;
                    if (SafeRead(bases[b] + static_cast<std::uintptr_t>(off), v) && v == fileSeed) {
                        g_seedOffset = off;
                        g_seedInLevel = b == 1;
                        g_seed32 = false;
                    } else if (static_cast<std::int64_t>(static_cast<std::int32_t>(fileSeed)) == fileSeed &&
                               SafeRead(bases[b] + static_cast<std::uintptr_t>(off), v32) && v32 == static_cast<std::int32_t>(fileSeed) &&
                               fileSeed > 65535) {
                        g_seedOffset = off;
                        g_seedInLevel = b == 1;
                        g_seed32 = true;
                    }
                    if (g_seedOffset >= 0) {
                        log::Info("World id: the seed is at {}+0x{:X} ({} bytes).", b == 1 ? "Level" : "LevelData", off, g_seed32 ? 4 : 8);
                        break;
                    }
                }
            }
            // Not there: one step further, inside the objects they point to.
            for (int b = 0; b < 2 && g_seedOffset == -2; ++b) {
                for (int poff = 0; poff < 0x2000 && g_seedOffset == -2; poff += 8) {
                    std::uintptr_t ptr = 0;
                    if (!SafeRead(bases[b] + static_cast<std::uintptr_t>(poff), ptr) || !LooksLikeDataPointer(ptr)) continue;
                    for (int off = 0; off < 0x800; off += 4) {
                        std::int64_t v = 0;
                        if (SafeRead(ptr + static_cast<std::uintptr_t>(off), v) && v == fileSeed) {
                            g_seedOffset = off;
                            g_seedPtr = poff;
                            g_seedInLevel = b == 1;
                            log::Info("World id: the seed is at {}+0x{:X} -> +0x{:X}.", b == 1 ? "Level" : "LevelData", poff, off);
                            break;
                        }
                    }
                }
            }
            // Still not: the game's own getter, read by hand (see SeedFromGetter).
            if (g_seedOffset == -2) {
                std::int64_t viaGetter = 0;
                if (SeedFromGetter(level, viaGetter, true) && viaGetter == fileSeed) {
                    g_seedOffset = -3;  // (-3 = read through the getter each time)
                    log::Info("World id: the seed is read through the game's getLevelSeed64 (Level +0x{:X}).", kLevelSeedSlot);
                }
            }
            if (g_seedOffset == -2 && ++g_seedTries >= 5) {
                g_seedOffset = -1;
                log::Warn("World id: the seed wasn't found in memory.");
            }
        }
    }
    if (g_seedOffset == -3) {
        std::int64_t got = 0;
        g_haveSeed = SeedFromGetter(level, got, false);
        g_seed = got;
    }
    if (g_seedOffset >= 0) {
        std::uintptr_t base = g_seedInLevel ? level : data;
        if (g_seedPtr >= 0 && (!SafeRead(base + static_cast<std::uintptr_t>(g_seedPtr), base) || !LooksLikeDataPointer(base))) base = 0;
        const std::uintptr_t at = base + static_cast<std::uintptr_t>(g_seedOffset);
        if (base == 0) {
            g_haveSeed = false;
        } else if (g_seed32) {
            std::int32_t v = 0;
            g_haveSeed = SafeRead(at, v);
            g_seed = v;
        } else {
            std::int64_t v = 0;
            g_haveSeed = SafeRead(at, v);
            g_seed = v;
        }
    }
}

const std::string& Folder() {
    return g_folder;
}

bool Seed(std::int64_t& out) {
    if (!g_haveSeed) return false;
    out = g_seed;
    return true;
}

}  // namespace luma::sdk::worldid
