// =============================================================================
//  blocks.cpp - Reading the world's blocks (see blocks.h)
// =============================================================================
#include "sdk/blocks.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/logger.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"

namespace luma::sdk::blocks {

namespace {

std::atomic<bool> g_wanted{false};

// --- Calling the game: BlockSource::getBlock (virtual +0x10) ------------------------
using GetBlockFn = void*(__fastcall*)(void* region, const int* pos);

void* GetBlock(void* region, int x, int y, int z) {
    const int pos[3] = {x, y, z};
    __try {
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(region);
        const auto fn = *reinterpret_cast<std::uintptr_t*>(vtable + 0x10);
        return reinterpret_cast<GetBlockFn>(fn)(region, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// --- Finding the names ------------------------------------------------------------------
// Where the name is: Block + legacyOffset -> BlockLegacy, + nameOffset -> std::string.
// (legacyOffset = -1: the string is in the Block itself.)
int g_legacyOffset = -2;  // -2 = not searched yet
int g_nameOffset = 0;
int g_attempts = 0;

// Does it look like a block name? "namespace:name": lowercase letters, numbers, _ . /
bool LooksLikeName(const char* s) {
    const std::size_t n = std::strlen(s);
    if (n < 5 || n > 64) return false;
    const char* colon = std::strchr(s, ':');
    if (colon == nullptr || colon == s || colon[1] == '\0' || std::strchr(colon + 1, ':') != nullptr) return false;
    for (const char* p = s; *p; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '.' || c == '/')) return false;
    }
    return true;
}

bool NameAt(std::uintptr_t block, int legacyOffset, int nameOffset, char* out, std::size_t size) {
    std::uintptr_t base = block;
    if (legacyOffset >= 0) {
        if (!SafeRead(block + static_cast<std::uintptr_t>(legacyOffset), base) || !LooksLikePointer(base)) return false;
    }
    return SafeReadString(base + static_cast<std::uintptr_t>(nameOffset), out, size) && LooksLikeName(out);
}

// Search the places where BOTH blocks have a name, and the names differ.
bool Discover(std::uintptr_t a, std::uintptr_t b) {
    char na[80], nb[80];
    for (int legacy = -1; legacy < 0x180; legacy += (legacy < 0 ? 1 : 8)) {
        for (int name = 0; name < 0x300; name += 8) {
            if (NameAt(a, legacy, name, na, sizeof(na)) && NameAt(b, legacy, name, nb, sizeof(nb)) && std::strcmp(na, nb) != 0) {
                g_legacyOffset = legacy;
                g_nameOffset = name;
                log::Info("Blocks: names found at Block+0x{:X} -> +0x{:X} (\"{}\", \"{}\").", legacy < 0 ? 0 : legacy, name, na, nb);
                return true;
            }
        }
    }
    return false;
}

// --- What each block is (by its address) ---------------------------------------------------
struct Kind {
    std::uint32_t rgb = 0x969696;
    bool air = false;   // nothing: keep going down
    bool skip = false;  // something you see through on a map (grass, flowers, torches...): keep going down
};
std::unordered_map<std::uintptr_t, Kind> g_kinds;

bool Has(const char* name, const char* part) {
    return std::strstr(name, part) != nullptr;
}

Kind Classify(const char* name) {
    Kind k;
    // Nothing there.
    if (Has(name, ":air") || Has(name, "cave_air") || Has(name, "void_air") || Has(name, "structure_void") ||
        Has(name, "light_block") || Has(name, "barrier")) {
        k.air = true;
        return k;
    }
    // Small things a map looks through.
    static const char* const kSee[] = {"short_grass", "tall_grass", "fern", "flower", "poppy", "dandelion", "tulip",
                                       "orchid", "allium", "bluet", "daisy", "torch", "rail", "button", "lever",
                                       "sapling", "mushroom", "vine", "dead_bush", "sugar_cane", "carpet", "pressure_plate",
                                       "tripwire", "redstone_wire", "glass_pane", "sign", "banner", "bush"};
    for (const char* s : kSee) {
        if (Has(name, s)) {
            k.skip = true;
            return k;
        }
    }
    struct Rule {
        const char* part;
        std::uint32_t rgb;
    };
    static const Rule kRules[] = {
        {"water", 0x2F5CDC}, {"lava", 0xE65A14}, {"grass_block", 0x6FAA3C}, {"moss", 0x5A7A2A},
        {"leaves", 0x3A7A24}, {"azalea", 0x5C8A30}, {"snow", 0xF2F6FA}, {"ice", 0x9AB8FF}, {"red_sand", 0xBE6428},
        {"sandstone", 0xD8C890}, {"sand", 0xDBCFA3}, {"gravel", 0x837D78}, {"podzol", 0x6A4828}, {"mycelium", 0x786878},
        {"farmland", 0x5E3E24}, {"path", 0x987848}, {"mud", 0x3C3434}, {"dirt", 0x866043}, {"clay", 0xA0A6B3},
        {"log", 0x6E5532}, {"wood", 0x6E5532}, {"stem", 0x5A3A50}, {"planks", 0xA08250}, {"bookshelf", 0x8C6E44},
        {"terracotta", 0xA05A3C}, {"netherrack", 0x6E2828}, {"nylium", 0x5A2830}, {"soul", 0x504030},
        {"end_stone", 0xDCDCA0}, {"obsidian", 0x140F1E}, {"bedrock", 0x3C3C3C}, {"deepslate", 0x4A4A50},
        {"cobble", 0x6E6E6E}, {"stone", 0x7A7A7A}, {"andesite", 0x848484}, {"diorite", 0xC0C0C0}, {"granite", 0x9A6A55},
        {"tuff", 0x6A6A60}, {"calcite", 0xDADADA}, {"basalt", 0x4A4A50}, {"blackstone", 0x2A2428}, {"glass", 0xC0E8F0},
        {"pumpkin", 0xD88020}, {"melon", 0x7AA030}, {"hay", 0xC8A830}, {"cactus", 0x5A8A30}, {"kelp", 0x3A7A40},
        {"coral", 0xD06090}, {"prismarine", 0x5AA090}, {"copper", 0xB06A48}, {"iron", 0xD8D8D8}, {"gold", 0xF0D040},
        {"diamond", 0x60E0D8}, {"emerald", 0x40C060}, {"quartz", 0xECE6DC}, {"brick", 0x985040},
    };
    // Colored blocks (wool, concrete, glass...): by their color word.
    static const Rule kColors[] = {
        {"light_blue", 0x6AA0E0}, {"light_gray", 0x9A9A9A}, {"white", 0xE8E8E8}, {"orange", 0xE07A20},
        {"magenta", 0xC050C0}, {"yellow", 0xE8D030}, {"lime", 0x70C030}, {"pink", 0xE890A8}, {"gray", 0x505050},
        {"cyan", 0x208890}, {"purple", 0x7A3AA8}, {"blue", 0x3040A0}, {"brown", 0x704830}, {"green", 0x4A6A20},
        {"red", 0xA02828}, {"black", 0x202020},
    };
    if (Has(name, "wool") || Has(name, "concrete") || Has(name, "stained") || Has(name, "shulker") || Has(name, "candle") ||
        Has(name, "bed")) {
        for (const Rule& r : kColors) {
            if (Has(name, r.part)) {
                k.rgb = r.rgb;
                return k;
            }
        }
    }
    for (const Rule& r : kRules) {
        if (Has(name, r.part)) {
            k.rgb = r.rgb;
            return k;
        }
    }
    return k;  // anything else: a neutral gray
}

const Kind* KindOf(void* block) {
    const auto key = reinterpret_cast<std::uintptr_t>(block);
    const auto it = g_kinds.find(key);
    if (it != g_kinds.end()) return &it->second;
    char name[80];
    Kind k;
    if (NameAt(key, g_legacyOffset, g_nameOffset, name, sizeof(name))) {
        k = Classify(name);
    }
    if (g_kinds.size() > 20000) g_kinds.clear();  // (never in practice: a safety net)
    return &(g_kinds[key] = k);
}

// --- The grid (wraps around: cell of world column x,z = [x & 127, z & 127]) ----------
struct Column {
    int x = 0x7FFFFFFF, z = 0;  // which world column it holds
    std::uint32_t rgb = 0;
    std::int16_t height = 0;
    bool known = false;
};
std::vector<Column> g_cells(static_cast<std::size_t>(kGrid) * kGrid);
int g_centerX = 0, g_centerZ = 0;
int g_cursor = 0;  // the next column to scan (cycles through the grid)
std::mutex g_mutex;
std::uint32_t g_version = 0;
float g_costUs = 0.0f;

LARGE_INTEGER Freq() {
    static LARGE_INTEGER f{};
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    return f;
}

bool PlayerFeet(int out[3]) {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t aabb = 0;
    float feet[3]{};
    if (player == 0 || !SafeRead(player + offsets::kActorAabb, aabb) || !LooksLikeDataPointer(aabb) || !SafeRead(aabb, feet)) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(feet[k]) || std::fabs(feet[k]) > 3.0e7f) return false;
        out[k] = static_cast<int>(std::floor(feet[k]));
    }
    return true;
}

}  // namespace

void Want(bool want) {
    g_wanted.store(want);
}

bool Wanted() {
    return g_wanted.load(std::memory_order_relaxed);
}

bool Available() {
    return g_legacyOffset >= -1;
}

void OnRegion(void* region) {
    if (!g_wanted.load(std::memory_order_relaxed) || region == nullptr || !LooksLikePointer(reinterpret_cast<std::uintptr_t>(region))) {
        return;
    }
    int feet[3];
    if (!PlayerFeet(feet)) return;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    // 1. The first time: find where the names are.
    if (g_legacyOffset == -2) {
        if (g_attempts++ > 600) return;  // (10 s of tries: then give up for this session)
        void* below = GetBlock(region, feet[0], feet[1] - 1, feet[2]);
        void* above = GetBlock(region, feet[0], feet[1] + 6, feet[2]);
        if (below == nullptr || above == nullptr || below == above) return;
        if (!Discover(reinterpret_cast<std::uintptr_t>(below), reinterpret_cast<std::uintptr_t>(above))) {
            if (g_attempts == 1 || g_attempts % 120 == 0) log::Warn("Blocks: couldn't find the block names yet (try {}).", g_attempts);
            return;
        }
    }

    // 2. This frame's share of columns (at most ~0.5 ms). The grid is locked
    // meanwhile (the overlay copies it from another thread).
    std::unique_lock gridLock(g_mutex);
    const int cx = feet[0], cz = feet[2];
    const int top = std::min(feet[1] + 40, 319);
    const int bottom = std::max(feet[1] - 80, -64);
    const LONGLONG budget = Freq().QuadPart / 2000;  // 0.5 ms
    int scanned = 0;
    bool changed = false;
    while (scanned < kGrid * kGrid) {
        const int i = g_cursor;
        g_cursor = (g_cursor + 1) % (kGrid * kGrid);
        ++scanned;
        const int wx = cx - kGrid / 2 + i % kGrid;
        const int wz = cz - kGrid / 2 + i / kGrid;
        Column& col = g_cells[static_cast<std::size_t>((wx & (kGrid - 1)) + (wz & (kGrid - 1)) * kGrid)];
        int start = top;
        if (col.known && col.x == wx && col.z == wz) start = std::min(top, col.height + 8);
        Column fresh;
        fresh.x = wx;
        fresh.z = wz;
        for (int y = start; y >= bottom; --y) {
            void* b = GetBlock(region, wx, y, wz);
            if (b == nullptr) break;
            const Kind* k = KindOf(b);
            if (k->air || k->skip) continue;
            fresh.rgb = k->rgb;
            fresh.height = static_cast<std::int16_t>(y);
            fresh.known = true;
            break;
        }
        if (fresh.known != col.known || fresh.rgb != col.rgb || fresh.height != col.height || fresh.x != col.x || fresh.z != col.z) {
            changed = true;
        }
        col = fresh;
        if ((scanned & 15) == 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart - t0.QuadPart > budget) break;
        }
    }
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const float us = static_cast<float>(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1.0e6 / static_cast<double>(Freq().QuadPart));
    g_costUs += (us - g_costUs) * 0.05f;
    g_centerX = cx;
    g_centerZ = cz;
    if (changed) ++g_version;
    static int logs = 0;
    if (++logs % 600 == 0) log::Info("Blocks: minimap work {:.0f} us/frame ({} kinds of blocks known).", g_costUs, g_kinds.size());
}

bool CopyGrid(std::uint32_t knownVersion, std::uint32_t* rgbaOut, int& centerX, int& centerZ, std::uint32_t& version) {
    std::lock_guard lock(g_mutex);
    if (g_version == knownVersion) return false;
    centerX = g_centerX;
    centerZ = g_centerZ;
    version = g_version;
    // Row by row, north (-Z) at the top. Each pixel a little darker or
    // lighter than its northern neighbor, like the paper maps of the game:
    // that's what makes hills visible.
    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            const int wx = centerX - kGrid / 2 + c, wz = centerZ - kGrid / 2 + r;
            const Column& col = g_cells[static_cast<std::size_t>((wx & (kGrid - 1)) + (wz & (kGrid - 1)) * kGrid)];
            std::uint32_t px = 0;
            if (col.known && col.x == wx && col.z == wz) {
                const Column& north = g_cells[static_cast<std::size_t>((wx & (kGrid - 1)) + ((wz - 1) & (kGrid - 1)) * kGrid)];
                float shade = 0.88f;
                if (north.known && north.x == wx && north.z == wz - 1) {
                    shade = col.height > north.height ? 1.0f : (col.height < north.height ? 0.74f : 0.88f);
                }
                const auto ch = [&](int shift) {
                    return static_cast<std::uint32_t>(std::clamp(static_cast<float>((col.rgb >> shift) & 0xFF) * shade, 0.0f, 255.0f));
                };
                px = ch(16) | (ch(8) << 8) | (ch(0) << 16) | 0xFF000000u;  // RGBA in memory (R first)
            }
            rgbaOut[r * kGrid + c] = px;
        }
    }
    return true;
}

float CostUs() {
    return g_costUs;
}

}  // namespace luma::sdk::blocks
