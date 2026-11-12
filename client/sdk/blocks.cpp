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
#include "sdk/scanner.h"

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

// --- BIOMES (part F) ----------------------------------------------------------------------
// In Minecraft grass, leaves and water are gray in their textures: the game
// tints them with the color of their BIOME. To know the biome of a block
// Luma asks the game itself: BlockSource::getBiome(BlockPos), a normal (not
// virtual) function, found by its first bytes ("signature"). How it was
// found (1.26.63): the scripting API's "getBiome" (Dimension.getBiome) is
// registered with a small "invoker" made for it; among the functions it
// calls there's one that asks the BlockSource for the chunk (its virtual
// +0x140, getChunkAt), checks the height is between the world's min and
// max (+0x3A, +0x38), and asks the chunk for the biome at (x & 15, y - min,
// z & 15); without a chunk, the dimension's default biome. That's it.
//
// The answer is a Biome object; its NAME ("plains", "savanna"...) is found
// the first time, like the block names: a text in the object that is one of
// the biomes of sdk/biome_colors.inc (made by scripts/gen_biome_colors.py
// from the game's files). The name gives the colors.
//
// If the signature isn't found (a game update), the maps keep the plains
// colors: nothing else changes.
struct BiomeColors {
    const char* name;
    std::uint32_t grass, foliage, water;
};
#include "sdk/biome_colors.inc"

constexpr const char* kGetBiomeSignature =
    "56 57 48 83 EC 28 48 89 D7 48 89 CE 48 8B 01 48 8B 80 40 01 00 00 FF 15 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 4F 04 0F BF 56 3A";
using GetBiomeFn = void*(__fastcall*)(void* region, const int* pos);
std::uintptr_t g_getBiome = 0;
std::atomic<bool> g_biomeTint{true};
int g_biomeNameOffset = -2;  // -2 = not searched yet, -1 = gave up
int g_biomeTries = 0;
std::unordered_map<std::uintptr_t, std::uint8_t> g_biomes;  // Biome object -> index in kBiomeColors (0xFF = unknown)
constexpr std::uint8_t kNoBiome = 0xFF;
std::mutex g_playerBiomeMutex;
std::string g_playerBiome;  // where you are (for F3)
int g_playerBiomeTick = 0;

void* GetBiomeObject(void* region, int x, int y, int z) {
    if (g_getBiome == 0) return nullptr;
    const int pos[3] = {x, y, z};
    __try {
        return reinterpret_cast<GetBiomeFn>(g_getBiome)(region, pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int BiomeByName(const char* full) {
    const char* name = std::strchr(full, ':') != nullptr ? std::strchr(full, ':') + 1 : full;
    for (std::size_t i = 0; i < sizeof(kBiomeColors) / sizeof(kBiomeColors[0]); ++i) {
        if (std::strcmp(kBiomeColors[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

// Which biome (index in kBiomeColors) a Biome object is.
std::uint8_t BiomeIndexOf(void* biome) {
    const auto key = reinterpret_cast<std::uintptr_t>(biome);
    if (key == 0 || !LooksLikePointer(key)) return kNoBiome;
    const auto it = g_biomes.find(key);
    if (it != g_biomes.end()) return it->second;
    char name[64];
    if (g_biomeNameOffset == -2) {
        // The first time: where in the object is its name?
        for (int off = 0; off <= 0x400; off += 8) {
            if (SafeReadString(key + static_cast<std::uintptr_t>(off), name, sizeof(name)) && name[0] != 0 && BiomeByName(name) >= 0) {
                g_biomeNameOffset = off;
                log::Info("Blocks: biome names found at Biome+0x{:X} (\"{}\").", off, name);
                break;
            }
        }
        if (g_biomeNameOffset == -2 && ++g_biomeTries >= 20) {
            g_biomeNameOffset = -1;
            log::Warn("Blocks: the biome names weren't found (plains colors for all).");
        }
        if (g_biomeNameOffset < 0) return kNoBiome;
    }
    if (g_biomeNameOffset < 0) return kNoBiome;
    std::uint8_t index = kNoBiome;
    if (SafeReadString(key + static_cast<std::uintptr_t>(g_biomeNameOffset), name, sizeof(name))) {
        const int i = BiomeByName(name);
        if (i >= 0) index = static_cast<std::uint8_t>(i);
        static int unknown = 0;
        if (i < 0 && unknown < 20) {
            ++unknown;
            log::Info("Blocks: biome \"{}\" isn't in the table (plains colors).", name);
        }
    }
    if (g_biomes.size() < 4096) g_biomes.emplace(key, index);
    return index;
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
    bool tree = false;  // leaves, logs, mushroom caps: NOT a roof (a tree over you isn't a cave)
    std::uint8_t tint = 0;  // tinted by biome: 0 no, 1 grass, 2 leaves, 3 water
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
    // Tinted by the biome? (In Bedrock birch, spruce, cherry, azalea and
    // mangrove leaves have their own fixed color: not by biome.)
    if (Has(name, "grass_block") || std::strcmp(name, "minecraft:grass") == 0) {
        k.tint = 1;
    } else if (Has(name, "leaves") && !Has(name, "birch") && !Has(name, "spruce") && !Has(name, "cherry") && !Has(name, "azalea") &&
               !Has(name, "mangrove")) {
        k.tint = 2;
    } else if (Has(name, "water")) {
        k.tint = 3;
    }
    k.tree = Has(name, "leaves") || Has(name, "_log") || Has(name, ":log") || Has(name, "wood") || Has(name, "_stem") ||
             Has(name, "hyphae") || Has(name, "mushroom_block") || Has(name, "wart_block") || Has(name, "azalea");
    // Nothing there.
    if (Has(name, ":air") || Has(name, "cave_air") || Has(name, "void_air") || Has(name, "structure_void") ||
        Has(name, "light_block") || Has(name, "barrier") ||
        Has(name, "placeholder")) {  // (the game's stand-in while a chunk is still arriving)
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
        // (Very light colors written to the log once: to find white patches that shouldn't be.)
        static int light = 0;
        const int lum = static_cast<int>(((game >> 16) & 0xFF) * 3 + ((game >> 8) & 0xFF) * 6 + (game & 0xFF)) / 10;
        if (lum > 225 && light < 40) {
            ++light;
            log::Info("Blocks: \"{}\" is very light (#{:06X}).", name, game);
        }
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
    // (Not found anywhere: written to the log once, to add it to the table.)
    static int logged = 0;
    if (logged < 60) {
        ++logged;
        log::Info("Blocks: no color for \"{}\" (gray).", name);
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
    bool rock = false;          // cave mode: solid rock at your level (drawn dark)
    std::int16_t layer = 0;     // cave mode: your height when it was read
    std::uint8_t tint = 0;      // its top block is tinted by biome (Kind::tint)
    std::uint8_t biome = 0xFF;  // which biome (index in kBiomeColors; 0xFF = not known)
    float scannedAt = -1.0e9f;  // when it was last read (seconds)
};
std::vector<Column> g_cells(static_cast<std::size_t>(kGrid) * kGrid);
// WHAT CHANGED since the last copy (CopyGrid): a few boxes of the world. Each
// changed column marks itself and its 8 neighbors (the light from the
// north-west and the cave walls look at neighbors). g_allDirty = everything.
std::vector<Area> g_dirty;
bool g_allDirty = true;
void MarkArea(int x0, int z0, int x1, int z1) {
    if (g_allDirty || x1 <= x0 || z1 <= z0) return;
    // Joined with a box that's close (so they stay few).
    for (Area& a : g_dirty) {
        if (x0 <= a.x1 + 8 && x1 >= a.x0 - 8 && z0 <= a.z1 + 8 && z1 >= a.z0 - 8) {
            a.x0 = std::min(a.x0, x0);
            a.z0 = std::min(a.z0, z0);
            a.x1 = std::max(a.x1, x1);
            a.z1 = std::max(a.z1, z1);
            return;
        }
    }
    if (g_dirty.size() >= 24) {
        // Too many: one box around all of them.
        Area all = g_dirty[0];
        for (const Area& a : g_dirty) {
            all.x0 = std::min(all.x0, a.x0);
            all.z0 = std::min(all.z0, a.z0);
            all.x1 = std::max(all.x1, a.x1);
            all.z1 = std::max(all.z1, a.z1);
        }
        g_dirty.clear();
        g_dirty.push_back(all);
        MarkArea(x0, z0, x1, z1);
        return;
    }
    g_dirty.push_back(Area{x0, z0, x1, z1});
}
void MarkColumn(int x, int z) {
    MarkArea(x - 1, z - 1, x + 2, z + 2);
}
int g_centerX = 0, g_centerZ = 0;
int g_cursor = 0;  // where the next frame continues looking (cycles through the grid)
// THE ORDER IN WHICH COLUMNS ARE LOOKED AT: from you outward (every cell of
// the grid, as an offset from you, sorted by distance). So the map fills
// around you first, and the far edge last.
std::vector<std::uint32_t> g_order;  // (dx + 256) | (dz + 256) << 16
// Each round goes out from a FIXED point (where you were when it started):
// if it followed you every frame, walking would leave rings of holes. It
// starts again from where you are when you're 48 blocks away from it.
int g_anchorX = 0, g_anchorZ = 0;
int g_nearCursor = 0;  // (the pass over what's near you, every frame)
bool g_haveAnchor = false;
void MakeOrder() {
    g_order.reserve(static_cast<std::size_t>(kGrid) * kGrid);
    for (int dz = -kGrid / 2; dz < kGrid / 2; ++dz) {
        for (int dx = -kGrid / 2; dx < kGrid / 2; ++dx) {
            g_order.push_back(static_cast<std::uint32_t>(dx + kGrid / 2) | (static_cast<std::uint32_t>(dz + kGrid / 2) << 16));
        }
    }
    std::stable_sort(g_order.begin(), g_order.end(), [](std::uint32_t a, std::uint32_t b) {
        const int ax = static_cast<int>(a & 0xFFFF) - kGrid / 2, az = static_cast<int>(a >> 16) - kGrid / 2;
        const int bx = static_cast<int>(b & 0xFFFF) - kGrid / 2, bz = static_cast<int>(b >> 16) - kGrid / 2;
        return ax * ax + az * az < bx * bx + bz * bz;
    });
}
std::mutex g_mutex;
std::uint32_t g_version = 0;
float g_costUs = 0.0f;

// --- CAVE MODE ---------------------------------------------------------------------------
// From above, a cave is invisible: the map would show the grass over your
// head. So when there's a ROOF over you (stone, dirt... not leaves or logs),
// the map shows instead the FLOOR of the cave at your level, and the rock
// around it dark. Twice a second Luma looks up from 5 columns (yours and 4
// around, 3 blocks away); a roof over at least 4 of them = you're under
// ground. In the Nether there's always a roof (the bedrock), so it's always on.
// To avoid flickering at a cave's mouth, it changes only after two equal
// looks in a row. When it changes, the whole grid is read again.
// How the terrain is painted (SetPainting).
float g_relief = 1.0f;
bool g_heightTint = true, g_waterDepth = true, g_shores = true, g_texture = true;

std::atomic<int> g_caveMode{0};       // 0 auto, 1 off, 2 always
std::atomic<bool> g_caveShown{false};  // what the minimap shows now
bool g_cave = false;
int g_caveVotes = 0;
float g_nextRoofCheck = 0.0f;

// The dimension (BlockSource's virtual function +0x90 writes it: the same one
// the game uses; sdk/dynlight too). Only trusted if the function is inside
// the game's own code.
std::atomic<int> g_dimension{-1};
int g_dimensionTick = 0;
// New starts (see blocks.h).
std::atomic<std::uint32_t> g_session{1}, g_worldSession{1};
std::uintptr_t g_lastPlayer = 0;
int g_lastDimension = -1;
using GetDimensionFn = int*(__fastcall*)(void* source, int* out);

void ReadDimension(void* region) {
    std::uintptr_t vtable = 0, fn = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(region), vtable) || !SafeRead(vtable + 0x90, fn) || fn < GameBase() ||
        fn >= GameBase() + 0x20000000) {
        return;
    }
    int d = -1;
    reinterpret_cast<GetDimensionFn>(fn)(region, &d);
    if (d >= 0 && d <= 2) g_dimension.store(d, std::memory_order_relaxed);
}

bool HasRoof(void* region, const int feet[3]) {
    static const int kAround[5][2] = {{0, 0}, {3, 0}, {-3, 0}, {0, 3}, {0, -3}};
    int roofs = 0;
    const int top = std::min(feet[1] + 128, 319);
    for (const auto& d : kAround) {
        for (int y = feet[1] + 2; y <= top; ++y) {
            void* b = GetBlock(region, feet[0] + d[0], y, feet[2] + d[1]);
            if (b == nullptr) break;
            const Kind* k = KindOf(b);
            if (k->air || k->skip || k->water || k->tree) continue;
            ++roofs;
            break;
        }
    }
    return roofs >= 4;
}

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

// Forgets everything read (a new world or dimension). Called with the grid locked.
void ResetLocked(bool newWorld) {
    for (Column& col : g_cells) col = Column{};
    g_allDirty = true;
    g_haveAnchor = false;
    g_cursor = 0;
    g_cave = false;
    g_caveVotes = 0;
    g_caveShown.store(false, std::memory_order_relaxed);
    g_nextRoofCheck = 0.0f;
    if (newWorld) {
        // A server can bring its own blocks: what each Block address means may change.
        g_kinds.clear();
        g_biomes.clear();
        g_worldSession.fetch_add(1, std::memory_order_relaxed);
    }
    g_session.fetch_add(1, std::memory_order_relaxed);
    ++g_version;
}

}  // namespace

std::uint32_t Session() {
    return g_session.load(std::memory_order_relaxed);
}

std::uint32_t WorldSession() {
    return g_worldSession.load(std::memory_order_relaxed);
}

namespace {
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

void Init() {
    const ScanResult r = FindPattern(kGetBiomeSignature);
    if (r.address != 0 && r.matches == 1) {
        g_getBiome = r.address;
        log::Info("Blocks: BlockSource::getBiome at game+0x{:X} (biome colors on).", r.address - GameBase());
    } else {
        log::Warn("Blocks: BlockSource::getBiome not found ({} matches): grass, leaves and water keep the plains colors.", r.matches);
    }
}

void SetBiomeTint(bool on) {
    if (g_biomeTint.exchange(on) != on) {
        std::lock_guard lock(g_mutex);
        g_allDirty = true;
        ++g_version;
    }
}

bool BiomeTintAvailable() {
    return g_getBiome != 0;
}

std::string PlayerBiome() {
    std::lock_guard lock(g_playerBiomeMutex);
    return g_playerBiome;
}

void SetPainting(float relief, bool heightTint, bool waterDepth, bool shores, bool texture) {
    std::lock_guard lock(g_mutex);
    if (relief == g_relief && heightTint == g_heightTint && waterDepth == g_waterDepth && shores == g_shores && texture == g_texture) return;
    g_relief = relief;
    g_heightTint = heightTint;
    g_waterDepth = waterDepth;
    g_shores = shores;
    g_texture = texture;
    g_allDirty = true;
    ++g_version;  // (the minimap copies and repaints)
}

void SetCaveMode(int mode) {
    g_caveMode.store(std::clamp(mode, 0, 2), std::memory_order_relaxed);
}

bool CaveActive() {
    return g_caveShown.load(std::memory_order_relaxed);
}

int Dimension() {
    return g_dimension.load(std::memory_order_relaxed);
}

void OnRegion(void* region) {
    if (!g_wanted.load(std::memory_order_relaxed) || region == nullptr || !LooksLikePointer(reinterpret_cast<std::uintptr_t>(region))) {
        return;
    }
    int feet[3];
    if (!PlayerFeet(feet)) return;
    // A new world (another local player) or another dimension: start again.
    {
        const std::uintptr_t player = sdk::player::LocalPlayer();
        const bool newWorld = player != g_lastPlayer;
        if (newWorld || g_dimensionTick++ % 4 == 0) ReadDimension(region);
        const int dim = g_dimension.load(std::memory_order_relaxed);
        const bool newDimension = dim >= 0 && g_lastDimension >= 0 && dim != g_lastDimension;
        if (newWorld || newDimension) {
            std::lock_guard lock(g_mutex);
            ResetLocked(newWorld);
            log::Info("Blocks: {} - starting the map again.", newWorld ? "another world" : "another dimension");
        }
        g_lastPlayer = player;
        if (dim >= 0) g_lastDimension = dim;
    }
    // (Once: where the BlockSource's table of functions is, for finding more of them.)
    static bool vtLogged = false;
    if (!vtLogged) {
        std::uintptr_t vt = 0;
        if (SafeRead(reinterpret_cast<std::uintptr_t>(region), vt)) {
            vtLogged = true;
            log::Info("Blocks: BlockSource table of functions at game+0x{:X}.", vt - GameBase());
        }
    }
    // The biome where you are (twice a second, for F3).
    if (g_getBiome != 0 && g_biomeNameOffset >= 0 && g_playerBiomeTick++ % 30 == 0) {
        void* b = GetBiomeObject(region, feet[0], feet[1], feet[2]);
        char name[64];
        if (b != nullptr && SafeReadString(reinterpret_cast<std::uintptr_t>(b) + static_cast<std::uintptr_t>(g_biomeNameOffset), name, sizeof(name))) {
            std::lock_guard lock(g_playerBiomeMutex);
            g_playerBiome = name;
        }
    }
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
    const float now = static_cast<float>(static_cast<double>(t0.QuadPart) / static_cast<double>(Freq().QuadPart));
    bool changed = false;
    // Cave mode: on or off? (see HasRoof)
    const int caveMode = g_caveMode.load(std::memory_order_relaxed);
    if (caveMode != 0 || now >= g_nextRoofCheck) {
        const bool want = caveMode == 2 || (caveMode == 0 && HasRoof(region, feet));
        if (caveMode == 0) g_nextRoofCheck = now + 0.5f;
        g_caveVotes = want == g_cave ? 0 : g_caveVotes + 1;
        if (g_caveVotes >= (caveMode == 0 ? 2 : 1)) {
            g_cave = want;
            g_caveVotes = 0;
            g_caveShown.store(want, std::memory_order_relaxed);
            for (Column& col : g_cells) col = Column{};  // read everything again, the other way
            g_allDirty = true;
            changed = true;
            log::Info("Blocks: cave mode {}.", want ? "on" : "off");
        }
    }
    const int cx = feet[0], cz = feet[2];
    const int top = std::min(feet[1] + 40, 319);
    const int bottom = -64;  // (all the way down: flying high, the ground can be far below)
    const LONGLONG budget = Freq().QuadPart / 2000;  // 0.5 ms
    int visited = 0, refreshed = 0;
    // EVERY FRAME, FIRST, what's near you (30 blocks around, ~2800 columns):
    // the big round can take a long while to come back, and a column read
    // while its chunk was still loading (after a portal, say) would stay
    // empty until then. Looking at a known column costs nothing.
    constexpr int kNear = 2828;
    int nearVisits = 0;
    while (visited < 16384) {  // (looking at a column costs nothing; reading it is what costs)
        if (g_order.empty()) MakeOrder();
        int wx = 0, wz = 0;
        if (nearVisits < kNear) {
            const std::uint32_t o = g_order[static_cast<std::size_t>(g_nearCursor)];
            g_nearCursor = (g_nearCursor + 1) % kNear;
            ++nearVisits;
            wx = cx + static_cast<int>(o & 0xFFFF) - kGrid / 2;
            wz = cz + static_cast<int>(o >> 16) - kGrid / 2;
        } else {
            if (!g_haveAnchor || std::abs(cx - g_anchorX) > 48 || std::abs(cz - g_anchorZ) > 48 || g_cursor == 0) {
                g_haveAnchor = true;
                g_anchorX = cx;
                g_anchorZ = cz;
                g_cursor = 0;
            }
            const std::uint32_t o = g_order[static_cast<std::size_t>(g_cursor)];
            g_cursor = (g_cursor + 1) % (kGrid * kGrid);
            wx = g_anchorX + static_cast<int>(o & 0xFFFF) - kGrid / 2;
            wz = g_anchorZ + static_cast<int>(o >> 16) - kGrid / 2;
        }
        ++visited;
        Column& col = g_cells[static_cast<std::size_t>((wx & (kGrid - 1)) + (wz & (kGrid - 1)) * kGrid)];
        const bool same = col.x == wx && col.z == wz;
        const float age = now - col.scannedAt;
        // (In a cave, also when you went up or down more than 3 blocks: your level changed.)
        const bool isNew = !same || (!col.known && age > 2.0f) || (g_cave && std::abs(col.layer - feet[1]) > 3);
        if (!isNew) {
            // Near you (12 blocks): every second, so what you place or break
            // shows at once. The rest: every 6 seconds.
            const int ddx = wx - cx, ddz = wz - cz;
            const bool isNear = ddx * ddx + ddz * ddz <= 12 * 12;
            if (age < (isNear ? 1.0f : 6.0f) || refreshed >= (isNear ? 96 : 24)) continue;
            ++refreshed;
        }
        Column fresh;
        fresh.x = wx;
        fresh.z = wz;
        fresh.scannedAt = now;
        fresh.layer = static_cast<std::int16_t>(feet[1]);
        if (g_cave) {
            // CAVE: from just over your head down. If it starts in solid
            // rock and stays solid down to 10 blocks under your feet, it's a
            // wall (dark). Otherwise: the first block under open space is
            // the cave's floor.
            const int ctop = std::min(feet[1] + 3, 319);
            // (The Nether: always "under a roof", with big open caves: looks deeper.)
            const int cbottom = std::max(feet[1] - (g_lastDimension == 1 ? 64 : 32), -64);
            bool open = false, any = false;
            for (int y = ctop; y >= cbottom; --y) {
                void* b = GetBlock(region, wx, y, wz);
                if (b == nullptr) break;
                any = true;
                const Kind* k = KindOf(b);
                if (k->air || k->skip) {
                    open = true;
                    continue;
                }
                if (!open) {
                    if (y <= feet[1] - 10) {  // solid all the way: rock
                        fresh.rgb = k->rgb;
                fresh.tint = k->tint;
                        fresh.height = static_cast<std::int16_t>(ctop);
                        fresh.known = fresh.rock = true;
                        break;
                    }
                    continue;  // still inside the ceiling
                }
                fresh.rgb = k->rgb;
                fresh.tint = k->tint;
                fresh.height = static_cast<std::int16_t>(y);
                fresh.known = true;
                if (k->water) {
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
            if (!any) fresh.known = false;
        }
        int start = top;
        if (col.known && same) start = std::min(top, col.height + 8);
        for (int y = start; y >= bottom && !g_cave; --y) {
            void* b = GetBlock(region, wx, y, wz);
            if (b == nullptr) break;
            const Kind* k = KindOf(b);
            if (k->air || k->skip) continue;
            fresh.rgb = k->rgb;
                fresh.tint = k->tint;
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
        // Its biome (only for grass, leaves and water; kept if it's the same column).
        if (fresh.known && fresh.tint != 0 && !fresh.rock && g_getBiome != 0 && g_biomeTint.load(std::memory_order_relaxed)) {
            fresh.biome = (same && col.biome != kNoBiome) ? col.biome : BiomeIndexOf(GetBiomeObject(region, wx, fresh.height, wz));
        }
        if (fresh.known != col.known || fresh.rgb != col.rgb || fresh.height != col.height || fresh.depth != col.depth ||
            fresh.rock != col.rock || fresh.biome != col.biome || !same) {
            changed = true;
            MarkColumn(wx, wz);
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
    // Walking: the strips that came into the grid were other columns before
    // (the grid is a ring): they must be copied again (as empty, until read).
    if (cx != g_centerX || cz != g_centerZ) {
        const int h = kGrid / 2;
        if (std::abs(cx - g_centerX) >= kGrid || std::abs(cz - g_centerZ) >= kGrid) {
            g_allDirty = true;
        } else {
            if (cx > g_centerX) MarkArea(g_centerX + h, cz - h, cx + h, cz + h);
            if (cx < g_centerX) MarkArea(cx - h, cz - h, g_centerX - h, cz + h);
            if (cz > g_centerZ) MarkArea(cx - h, g_centerZ + h, cx + h, cz + h);
            if (cz < g_centerZ) MarkArea(cx - h, cz - h, cx + h, g_centerZ - h);
        }
        changed = true;
    }
    g_centerX = cx;
    g_centerZ = cz;
    if (changed) ++g_version;
    static int logs = 0;
    if (++logs % 600 == 0) log::Info("Blocks: minimap work {:.0f} us/frame ({} kinds of blocks known).", g_costUs, g_kinds.size());
}

bool CopyGrid(std::uint32_t knownVersion, std::uint32_t* ringOut, int& centerX, int& centerZ, std::uint32_t& version,
              std::vector<Area>& areas, std::uint32_t* session, int* dimension) {
    std::lock_guard lock(g_mutex);
    if (g_version == knownVersion) return false;
    if (session != nullptr) *session = g_session.load(std::memory_order_relaxed);
    if (dimension != nullptr) *dimension = g_lastDimension;
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
    // One column, colored and shaded (0 = nothing known there).
    auto paint = [&](int wx, int wz) -> std::uint32_t {
            const Column* col = cellAt(wx, wz);
            std::uint32_t px = 0;
            if (col != nullptr) {
                float rgb[3] = {static_cast<float>((col->rgb >> 16) & 0xFF), static_cast<float>((col->rgb >> 8) & 0xFF),
                                static_cast<float>(col->rgb & 0xFF)};
                {
                    // Snow and other almost-white blocks: a little below white, or
                    // the light from the north-west would have nothing to brighten
                    // (the relief would vanish and it would look flat).
                    const float lum = rgb[0] * 0.3f + rgb[1] * 0.59f + rgb[2] * 0.11f;
                    if (lum > 215.0f) {
                        for (float& ch : rgb) ch *= 215.0f / lum;
                    }
                }
                // BIOME COLORS: the table's grass/leaves were painted with the
                // plains tint; turn that into this biome's (channel by channel).
                const bool tinted = col->tint != 0 && col->biome != kNoBiome && g_biomeTint.load(std::memory_order_relaxed);
                if (tinted && col->tint != 3) {
                    const BiomeColors& bc = kBiomeColors[col->biome];
                    const std::uint32_t want = col->tint == 1 ? bc.grass : bc.foliage;
                    const float base[3] = {col->tint == 1 ? 121.0f : 89.0f, col->tint == 1 ? 192.0f : 174.0f, col->tint == 1 ? 90.0f : 48.0f};
                    const float w[3] = {static_cast<float>((want >> 16) & 0xFF), static_cast<float>((want >> 8) & 0xFF),
                                        static_cast<float>(want & 0xFF)};
                    for (int k = 0; k < 3; ++k) rgb[k] = std::clamp(rgb[k] * w[k] / base[k], 0.0f, 255.0f);
                }
                float shade = 1.0f;
                if (col->rock) {
                    // Cave mode: the solid rock around the cave is black, so the
                    // open space (the cave itself) is what stands out.
                    rgb[0] = rgb[1] = rgb[2] = 8.0f;
                } else if (col->depth > 0 && g_waterDepth) {
                    // Water: from light turquoise (1 block) to deep blue (12+).
                    const float t = std::clamp((static_cast<float>(col->depth) - 1.0f) / 11.0f, 0.0f, 1.0f);
                    float shallow[3] = {86.0f, 170.0f, 230.0f}, deep[3] = {22.0f, 48.0f, 140.0f};
                    if (tinted && col->tint == 3) {
                        // This biome's water: lighter where shallow, darker where deep.
                        const std::uint32_t wc = kBiomeColors[col->biome].water;
                        const float w[3] = {static_cast<float>((wc >> 16) & 0xFF), static_cast<float>((wc >> 8) & 0xFF),
                                            static_cast<float>(wc & 0xFF)};
                        for (int k = 0; k < 3; ++k) {
                            shallow[k] = std::min(255.0f, w[k] * 1.12f + 14.0f);
                            deep[k] = w[k] * 0.33f;
                        }
                    }
                    for (int k = 0; k < 3; ++k) rgb[k] = shallow[k] + (deep[k] - shallow[k]) * t;
                    // Waves: a faint pattern.
                    const unsigned h = static_cast<unsigned>(wx * 73856093) ^ static_cast<unsigned>(wz * 19349663);
                    shade = 0.96f + static_cast<float>(h % 9) * 0.01f;
                } else {
                    const Column* north = cellAt(wx, wz - 1);
                    const Column* west = cellAt(wx - 1, wz);
                    // (A neighbor that is rock doesn't count: a cave's wall isn't a hill.)
                    const auto usable = [](const Column* n) { return n != nullptr && n->depth == 0 && !n->rock; };
                    const float hn = usable(north) ? north->height : col->height;
                    const float hw = usable(west) ? west->height : col->height;
                    const float slope = (static_cast<float>(col->height) - hn) + (static_cast<float>(col->height) - hw);
                    if (g_cave) {
                        // IN A CAVE, the relief has to be read at a glance:
                        //  - stronger light from the north-west;
                        //  - a dark LINE on the low side of every step (each block
                        //    of height draws a contour, like a relief map);
                        //  - the deeper under your feet, the darker;
                        //  - next to the rock (the cave's wall), a darker rim.
                        shade = std::clamp(1.0f + slope * 0.16f * g_relief, 0.5f, 1.45f);
                        const Column* east = cellAt(wx + 1, wz);
                        const Column* south = cellAt(wx, wz + 1);
                        bool step = false, wall = false;
                        for (const Column* n : {north, west, east, south}) {
                            if (n == nullptr) continue;
                            if (n->rock) wall = true;
                            else if (n->depth == 0 && n->height > col->height) step = true;
                        }
                        if (step) shade *= 0.8f;
                        if (wall) shade *= 0.78f;
                        const float below = static_cast<float>(col->layer - 1 - col->height);
                        shade *= std::clamp(1.0f - below * 0.03f, 0.45f, 1.1f);
                    } else {
                        shade = std::clamp(1.0f + slope * 0.09f * g_relief, 0.4f, 1.5f);
                        if (g_heightTint) shade *= std::clamp(1.0f + (static_cast<float>(col->height) - 64.0f) * 0.004f, 0.8f, 1.15f);
                    }
                    const unsigned h = static_cast<unsigned>(wx * 73856093) ^ static_cast<unsigned>(wz * 19349663) ^ 0x9E3779B9u;
                    if (g_texture) shade *= 0.95f + static_cast<float>((h >> 7) % 11) * 0.01f;
                    // The edge of water: a slightly darker rim on land.
                    if (g_shores && col->depth == 0 && ((north != nullptr && north->depth > 0) || (west != nullptr && west->depth > 0))) shade *= 0.88f;
                }
                const auto ch = [&](int k) { return static_cast<std::uint32_t>(std::clamp(rgb[k] * shade, 0.0f, 255.0f)); };
                px = ch(0) | (ch(1) << 8) | (ch(2) << 16) | 0xFF000000u;  // RGBA in memory (R first)
            }
            return px;
    };
    // What to copy: what changed (or everything), inside the window.
    const int h = kGrid / 2;
    areas.clear();
    if (g_allDirty) {
        areas.push_back(Area{centerX - h, centerZ - h, centerX + h, centerZ + h});
    } else {
        for (const Area& a : g_dirty) {
            Area c{std::max(a.x0, centerX - h), std::max(a.z0, centerZ - h), std::min(a.x1, centerX + h), std::min(a.z1, centerZ + h)};
            if (c.x1 > c.x0 && c.z1 > c.z0) areas.push_back(c);
        }
    }
    g_dirty.clear();
    g_allDirty = false;
    for (const Area& a : areas) {
        for (int wz = a.z0; wz < a.z1; ++wz) {
            std::uint32_t* row = ringOut + static_cast<std::size_t>(wz & (kGrid - 1)) * kGrid;
            for (int wx = a.x0; wx < a.x1; ++wx) row[wx & (kGrid - 1)] = paint(wx, wz);
        }
    }
    return true;
}

float CostUs() {
    return g_costUs;
}

}  // namespace luma::sdk::blocks
