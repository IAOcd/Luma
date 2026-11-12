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
    bool water = false; // water: the map shows how deep it is
    bool own = false;   // the color comes from Luma's table (else: the game's map color, or gray)
};
std::unordered_map<std::uintptr_t, Kind> g_kinds;

bool Has(const char* name, const char* part) {
    return std::strstr(name, part) != nullptr;
}

// --- Every block's color, from the game's own textures --------------------------------
// client/sdk/block_colors.inc is made by scripts/gen_block_colors.py: the
// average color of each block's top texture (1300+ blocks, every version).
// Lookup: the exact name first; then the textures whose name has all the
// words of the block ("orange_wool" -> "wool_colored_orange"), the shortest
// one; a few renamed blocks by hand. Done once per kind of block.
struct BlockColor {
    const char* name;
    std::uint32_t rgb;
};
#include "sdk/block_colors.inc"

struct ColorIndex {
    std::unordered_map<std::string, std::uint32_t> blocks, textures;
    std::vector<std::pair<std::vector<std::string>, std::uint32_t>> words;  // texture -> its words
};

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t j = s.find('_', i);
        const std::string w = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!w.empty()) out.push_back(w);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

const ColorIndex& Index() {
    static const ColorIndex index = [] {
        ColorIndex x;
        for (const BlockColor& b : kBlockColors) x.blocks.emplace(b.name, b.rgb);
        for (const BlockColor& t : kTextureColors) {
            x.textures.emplace(t.name, t.rgb);
            x.words.emplace_back(Words(t.name), t.rgb);
        }
        return x;
    }();
    return index;
}

bool GameColor(const char* fullName, std::uint32_t& rgb) {
    const ColorIndex& x = Index();
    std::string name = fullName;
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    // Renamed blocks (the textures file keeps their old names).
    static const std::pair<const char*, const char*> kAliases[] = {
        {"grass_block", "grass"}, {"dirt_path", "grass_path"}, {"oak_log", "log"}, {"oak_planks", "planks"},
        {"oak_leaves", "leaves"}, {"stone_bricks", "stonebrick"}, {"bricks", "brick_block"}, {"sea_lantern", "seaLantern"},
        {"magma", "magma"}, {"terracotta", "hardened_clay"}, {"snow_layer", "snow_layer"}, {"powder_snow", "snow"},
    };
    for (const auto& [from, to] : kAliases) {
        if (name == from) {
            const auto it = x.blocks.find(to);
            if (it != x.blocks.end()) {
                rgb = it->second;
                return true;
            }
        }
    }
    if (const auto it = x.blocks.find(name); it != x.blocks.end()) {
        rgb = it->second;
        return true;
    }
    if (const auto it = x.textures.find(name); it != x.textures.end()) {
        rgb = it->second;
        return true;
    }
    // Shapes made of a material ("oak_stairs", "stone_brick_slab"): the material's words.
    std::vector<std::string> words = Words(name);
    static const char* const kShapes[] = {"stairs", "slab", "double", "wall", "fence", "gate", "door", "trapdoor",
                                          "button", "pressure", "plate", "block", "lit", "waxed"};
    std::vector<std::string> wanted;
    for (const std::string& w : words) {
        bool shape = false;
        for (const char* sh : kShapes) shape |= w == sh;
        if (!shape) wanted.push_back(w);
    }
    if (wanted.empty()) wanted = words;
    // The texture with all those words (a word may be part of a longer one:
    // "brick" in "bricks"), the one with fewest extra words; "top" textures win.
    int best = 1 << 20;
    std::uint32_t bestRgb = 0;
    for (const auto& [twords, color] : x.words) {
        bool all = true;
        for (const std::string& w : wanted) {
            bool found = false;
            for (const std::string& t : twords) found |= t.find(w) != std::string::npos || w.find(t) != std::string::npos && t.size() >= 4;
            if (!found) {
                all = false;
                break;
            }
        }
        if (!all) continue;
        int score = static_cast<int>(twords.size()) * 4;
        for (const std::string& t : twords) {
            if (t == "top") score -= 3;
            if (t == "side" || t == "bottom" || t == "carried" || t == "inner") score += 2;
        }
        if (score < best) {
            best = score;
            bestRgb = color;
        }
    }
    if (best < (1 << 20)) {
        rgb = bestRgb;
        return true;
    }
    return false;
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
    static const char* const kSee[] = {"short_grass", "tall_grass", "tallgrass", "fern", "torch", "rail", "button", "lever",
                                       "sapling", "vine", "dead_bush", "pressure_plate", "tripwire", "redstone_wire",
                                       "glass_pane", "sign", "banner", "bush", "seagrass", "kelp"};
    for (const char* s : kSee) {
        if (Has(name, s)) {
            k.skip = true;
            return k;
        }
    }
    // The game's own color (from its textures), for every block it knows.
    std::uint32_t game = 0;
    if (GameColor(name, game)) {
        k.rgb = game;
        k.own = true;
        k.water = Has(name, "water");
        return k;
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
                k.own = true;
                return k;
            }
        }
    }
    for (const Rule& r : kRules) {
        if (Has(name, r.part)) {
            k.rgb = r.rgb;
            k.water = std::strcmp(r.part, "water") == 0;
            k.own = true;
            return k;
        }
    }
    return k;  // anything else: a neutral gray
}

// --- The game's own MAP COLOR ----------------------------------------------------------
// Every kind of block (BlockLegacy) keeps the color the game uses for the
// paper maps: 4 numbers 0..1 (red, green, blue, opacity). Luma uses it for
// every block its own table doesn't know, so NO block is left gray (also the
// ones a future update adds). Where it is isn't known in this version, so
// it's FOUND: among the kinds of blocks already seen, the place where grass
// is green, water is blue, sand is light yellow and stone is gray, all at
// the same offset, is the map color. What it finds goes to the log.
int g_colorOffset = -1;  // (off: in this version the map color is not a plain field; the textures table is used instead)
std::unordered_map<std::string, std::uintptr_t> g_legacyByName;  // a few known kinds (for the search)

// A color as 4 floats 0..1 (red, green, blue, opacity).
bool ReadColor(std::uintptr_t base, int offset, float c[4]) {
    if (!SafeCopy(base + static_cast<std::uintptr_t>(offset), c, sizeof(float) * 4)) return false;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(c[i]) || c[i] < 0.0f || c[i] > 1.0001f) return false;
    }
    return c[0] + c[1] + c[2] > 0.05f;
}

// Where the color is: in the BlockLegacy (true) or in the Block itself (false).
bool g_colorInLegacy = true;
std::unordered_map<std::string, std::uintptr_t> g_blockByName;  // a few known Blocks (for the search)
int g_colorTries = 0;

bool LooksLike(const char* what, const float c[4]) {
    const float r = c[0], g = c[1], b = c[2];
    if (std::strcmp(what, "grass") == 0) return g > r + 0.06f && g > b + 0.06f;
    if (std::strcmp(what, "water") == 0) return b > r + 0.15f && b > g + 0.05f;
    if (std::strcmp(what, "sand") == 0) return r > 0.65f && g > 0.55f && b < r && b < g;
    if (std::strcmp(what, "stone") == 0) return std::fabs(r - g) < 0.08f && std::fabs(g - b) < 0.08f && r > 0.25f && r < 0.75f;
    if (std::strcmp(what, "dirt") == 0) return r > g && g > b && r > 0.3f;
    return false;
}

void FindMapColor() {
    struct Sample {
        const char* what;
        const char* name;
    };
    const Sample kSamples[] = {{"grass", "minecraft:grass_block"}, {"water", "minecraft:water"}, {"sand", "minecraft:sand"},
                               {"stone", "minecraft:stone"},       {"dirt", "minecraft:dirt"}};
    for (int pass = 0; pass < 2; ++pass) {
        const bool inLegacy = pass == 0;
        const auto& table = inLegacy ? g_legacyByName : g_blockByName;
        std::vector<std::pair<const char*, std::uintptr_t>> have;
        for (const Sample& smp : kSamples) {
            const auto it = table.find(smp.name);
            if (it != table.end()) have.emplace_back(smp.what, it->second);
        }
        if (have.size() < 3 || std::strcmp(have[0].first, "grass") != 0) return;  // grass + 2 others: wait for more
        const int limit = inLegacy ? 0x1000 : 0x400;
        for (int off = 0; off < limit; off += 4) {
            bool ok = true;
            float c[4];
            for (const auto& [what, base] : have) {
                if (!ReadColor(base, off, c) || !LooksLike(what, c)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            ReadColor(have[0].second, off, c);
            g_colorOffset = off;
            g_colorInLegacy = inLegacy;
            log::Info("Blocks: the game's map color found at {}+0x{:X} (grass {:.2f} {:.2f} {:.2f}, checked with {} blocks).",
                      inLegacy ? "BlockLegacy" : "Block", off, c[0], c[1], c[2], have.size());
            g_kinds.clear();  // classify again, now with the map colors
            return;
        }
    }
    // Not found with these blocks: try again when more kinds are known (a few times).
    if (++g_colorTries >= 6) {
        g_colorOffset = -1;
        // A clue for the next try: colors that look green in the grass's BlockLegacy.
        const auto it = g_legacyByName.find("minecraft:grass_block");
        if (it != g_legacyByName.end()) {
            int shown = 0;
            for (int off = 0; off < 0x1000 && shown < 12; off += 4) {
                float c[4];
                if (ReadColor(it->second, off, c) && LooksLike("grass", c)) {
                    log::Info("Blocks: greenish at BlockLegacy+0x{:X}: {:.3f} {:.3f} {:.3f} {:.3f}", off, c[0], c[1], c[2], c[3]);
                    ++shown;
                }
            }
        }
        log::Warn("Blocks: the game's map color wasn't found (unknown blocks stay gray).");
    }
}

const Kind* KindOf(void* block) {
    const auto key = reinterpret_cast<std::uintptr_t>(block);
    const auto it = g_kinds.find(key);
    if (it != g_kinds.end()) return &it->second;
    char name[80];
    Kind k;
    if (NameAt(key, g_legacyOffset, g_nameOffset, name, sizeof(name))) {
        k = Classify(name);
        std::uintptr_t legacy = 0;
        if (g_legacyOffset >= 0 && SafeRead(key + static_cast<std::uintptr_t>(g_legacyOffset), legacy) && LooksLikePointer(legacy)) {
            if (g_colorOffset == -2 && g_legacyByName.size() < 256) {
                const bool fresh = g_legacyByName.emplace(name, legacy).second;
                g_blockByName.emplace(name, key);
                if (fresh) FindMapColor();
            }
            // Not in Luma's table: the game's own map color.
            float c[4];
            if (!k.own && !k.air && !k.skip && g_colorOffset >= 0 && ReadColor(g_colorInLegacy ? legacy : key, g_colorOffset, c)) {
                k.rgb = (static_cast<std::uint32_t>(c[0] * 255.0f) << 16) | (static_cast<std::uint32_t>(c[1] * 255.0f) << 8) |
                        static_cast<std::uint32_t>(c[2] * 255.0f);
            }
        }
    }
    if (g_kinds.size() > 20000) g_kinds.clear();  // (never in practice: a safety net)
    return &(g_kinds[key] = k);
}

// --- The grid (wraps around: cell of world column x,z = [x & 127, z & 127]) ----------
struct Column {
    int x = 0x7FFFFFFF, z = 0;  // which world column it holds
    std::uint32_t rgb = 0;
    std::int16_t height = 0;
    std::uint8_t depth = 0;     // water: how many blocks deep (0 = not water)
    bool known = false;         // something was found (false = unloaded chunk or only air)
    float scannedAt = -1.0e9f;  // when it was last read (seconds)
};
std::vector<Column> g_cells(static_cast<std::size_t>(kGrid) * kGrid);
int g_centerX = 0, g_centerZ = 0;
int g_cursor = 0;  // where the next frame continues looking (cycles through the grid)
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

    // 2. This frame's share of columns. The grid is locked meanwhile (the
    // overlay copies it from another thread).
    //
    // LIGHT ON PURPOSE: a column is only read again when it's NEW (you
    // walked and it entered the map), when it was empty (a chunk that
    // wasn't loaded yet, tried again every 2 s), or every 6 s to notice
    // blocks placed or broken. New columns get up to 0.5 ms per frame (so
    // the map fills fast); refreshing old ones, at most 24 per frame.
    std::unique_lock gridLock(g_mutex);
    const int cx = feet[0], cz = feet[2];
    const int top = std::min(feet[1] + 40, 319);
    const int bottom = std::max(feet[1] - 80, -64);
    const LONGLONG budget = Freq().QuadPart / 2000;  // 0.5 ms
    const float now = static_cast<float>(static_cast<double>(t0.QuadPart) / static_cast<double>(Freq().QuadPart));
    int visited = 0, refreshed = 0;
    bool changed = false;
    while (visited < 4096) {  // (looking at a column costs nothing; reading it is what costs)
        const int i = g_cursor;
        g_cursor = (g_cursor + 1) % (kGrid * kGrid);
        ++visited;
        const int wx = cx - kGrid / 2 + i % kGrid;
        const int wz = cz - kGrid / 2 + i / kGrid;
        Column& col = g_cells[static_cast<std::size_t>((wx & (kGrid - 1)) + (wz & (kGrid - 1)) * kGrid)];
        const bool same = col.x == wx && col.z == wz;
        const float age = now - col.scannedAt;
        const bool isNew = !same || (!col.known && age > 2.0f);
        if (!isNew) {
            // Near you (12 blocks): every second, so what you place or break
            // shows at once. The rest: every 6 seconds.
            const int ddx = wx - cx, ddz = wz - cz;
            const bool isNear = ddx * ddx + ddz * ddz <= 12 * 12;
            if (age < (isNear ? 1.0f : 6.0f) || refreshed >= (isNear ? 96 : 24)) continue;
            ++refreshed;
        }
        int start = top;
        if (col.known && same) start = std::min(top, col.height + 8);
        Column fresh;
        fresh.x = wx;
        fresh.z = wz;
        fresh.scannedAt = now;
        for (int y = start; y >= bottom; --y) {
            void* b = GetBlock(region, wx, y, wz);
            if (b == nullptr) break;
            const Kind* k = KindOf(b);
            if (k->air || k->skip) continue;
            fresh.rgb = k->rgb;
            fresh.height = static_cast<std::int16_t>(y);
            fresh.known = true;
            if (k->water) {
                // How deep: count the water below (up to 16).
                int d = 1;
                while (d < 16) {
                    void* under = GetBlock(region, wx, y - d, wz);
                    if (under == nullptr || !KindOf(under)->water) break;
                    ++d;
                }
                fresh.depth = static_cast<std::uint8_t>(d);
            }
            break;
        }
        if (fresh.known != col.known || fresh.rgb != col.rgb || fresh.height != col.height || fresh.depth != col.depth || !same) {
            changed = true;
        }
        col = fresh;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (t.QuadPart - t0.QuadPart > budget) break;
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
    // Row by row, north (-Z) at the top. To make it look alive (not flat):
    //   - LIGHT FROM THE NORTH-WEST ("hillshade"): a column higher than its
    //     north and west neighbors is lit, a lower one is in shadow, and the
    //     more the difference, the stronger. Hills and cliffs pop out.
    //   - HEIGHT: higher ground a little lighter, deep valleys a little darker.
    //   - WATER BY DEPTH: shallow water light turquoise, deep water dark blue
    //     (and the shore stands out).
    //   - TEXTURE: each block a tiny bit lighter or darker (always the same
    //     one), like the pixels of a real texture.
    auto cellAt = [&](int wx, int wz) -> const Column* {
        const Column& c = g_cells[static_cast<std::size_t>((wx & (kGrid - 1)) + (wz & (kGrid - 1)) * kGrid)];
        return c.known && c.x == wx && c.z == wz ? &c : nullptr;
    };
    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            const int wx = centerX - kGrid / 2 + c, wz = centerZ - kGrid / 2 + r;
            const Column* col = cellAt(wx, wz);
            std::uint32_t px = 0;
            if (col != nullptr) {
                float rgb[3] = {static_cast<float>((col->rgb >> 16) & 0xFF), static_cast<float>((col->rgb >> 8) & 0xFF),
                                static_cast<float>(col->rgb & 0xFF)};
                float shade = 1.0f;
                if (col->depth > 0) {
                    // Water: from light turquoise (1 block) to deep blue (12+).
                    const float t = std::clamp((static_cast<float>(col->depth) - 1.0f) / 11.0f, 0.0f, 1.0f);
                    const float shallow[3] = {86.0f, 170.0f, 230.0f}, deep[3] = {22.0f, 48.0f, 140.0f};
                    for (int k = 0; k < 3; ++k) rgb[k] = shallow[k] + (deep[k] - shallow[k]) * t;
                    // Waves: a faint pattern.
                    const unsigned h = static_cast<unsigned>(wx * 73856093) ^ static_cast<unsigned>(wz * 19349663);
                    shade = 0.96f + static_cast<float>(h % 9) * 0.01f;
                } else {
                    const Column* north = cellAt(wx, wz - 1);
                    const Column* west = cellAt(wx - 1, wz);
                    const float hn = north != nullptr && north->depth == 0 ? north->height : col->height;
                    const float hw = west != nullptr && west->depth == 0 ? west->height : col->height;
                    const float slope = (static_cast<float>(col->height) - hn) + (static_cast<float>(col->height) - hw);
                    shade = std::clamp(1.0f + slope * 0.09f, 0.55f, 1.3f);
                    shade *= std::clamp(1.0f + (static_cast<float>(col->height) - 64.0f) * 0.004f, 0.8f, 1.15f);
                    const unsigned h = static_cast<unsigned>(wx * 73856093) ^ static_cast<unsigned>(wz * 19349663) ^ 0x9E3779B9u;
                    shade *= 0.95f + static_cast<float>((h >> 7) % 11) * 0.01f;
                    // The edge of water: a slightly darker rim on land.
                    if ((north != nullptr && north->depth > 0) || (west != nullptr && west->depth > 0)) shade *= 0.88f;
                }
                const auto ch = [&](int k) { return static_cast<std::uint32_t>(std::clamp(rgb[k] * shade, 0.0f, 255.0f)); };
                px = ch(0) | (ch(1) << 8) | (ch(2) << 16) | 0xFF000000u;  // RGBA in memory (R first)
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
