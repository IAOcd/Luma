// =============================================================================
//  blocks.h - Reading the world's blocks (for the minimap, Phase 11)
// =============================================================================
//  The game answers "which block is at (x, y, z)?" with BlockSource::getBlock
//  (its virtual function +0x10, the same one sdk/draw3d uses). It returns a
//  "Block": one object per kind of block and state (oak log facing north,
//  oak log facing east...). Those objects live as long as the game, so Luma
//  can remember what each one is by its address.
//
//  WHAT IS EACH BLOCK? The Block points to its "BlockLegacy" (the kind of
//  block), which keeps its name, like "minecraft:grass_block". We don't know
//  where those two things are in this version, so Luma FINDS them by itself
//  the first time: it looks through the Block's memory for pointers, and
//  through each pointed object for a text shaped like "namespace:name". Two
//  different blocks (the one under your feet and the air above you) must
//  agree on the same places. What it finds goes to the log.
//
//  WHEN: always inside the game's own drawing thread (sdk/draw3d calls
//  OnRegion every frame with the game's BlockSource): reading blocks from
//  another thread could crash the game if a chunk unloads at that moment.
//
//  If something doesn't look right, Available() stays false and the
//  minimap shows nothing. Never a crash.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::sdk::blocks {

// Finds BlockSource::getBiome by its signature (call once at start, on
// Luma's own thread: scanning the game's code takes a moment).
void Init();
// Grass, leaves and water in the color of their biome (true) or of plains.
void SetBiomeTint(bool on);
bool BiomeTintAvailable();
// The biome where you are ("" if unknown), for the debug screen (F3).
std::string PlayerBiome();

// Who wants blocks read (the work only happens while someone does).
void Want(bool want);
bool Wanted();

// The block names were found (the minimap can work).
bool Available();

// Called by sdk/draw3d every frame, on the game's drawing thread, with the
// game's BlockSource. Does this frame's share of the minimap's work.
void OnRegion(void* region);

// How the terrain is painted (modules/world/map_look.h): relief strength (0 =
// flat), height tint, water by depth, shores, texture. A change repaints.
void SetPainting(float relief, bool heightTint, bool waterDepth, bool shores, bool texture);

// Cave mode (see blocks.cpp): 0 = automatic (when there's a roof over you),
// 1 = never, 2 = always. CaveActive(): what the grid shows now.
void SetCaveMode(int mode);
bool CaveActive();

// Which dimension the game is drawing: 0 Overworld, 1 Nether, 2 End, -1 not
// known (asked to the BlockSource once a second, like sdk/dynlight does).
int Dimension();

// A NEW START: every time you enter another world (the local player changes)
// or another dimension, everything read so far is forgotten (the same x, z
// is another place there!). These count those starts: whoever keeps a copy
// of the grid must drop it when they change. WorldSession only counts new
// worlds (not dimension changes).
std::uint32_t Session();
std::uint32_t WorldSession();

// --- The minimap's grid (filled by OnRegion) --------------------------------------
inline constexpr int kGrid = 512;  // blocks per side (256 on each side of you: the minimap reaches 200)

struct Cell {
    std::uint32_t rgba = 0;  // 0 = not known yet (unloaded chunk or not scanned)
    std::int16_t height = 0;
};

// A part of the world (blocks x0 <= x < x1, z0 <= z < z1).
struct Area {
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
};

// ONLY WHAT CHANGED: copies (colored and shaded) the columns that changed
// since the last call, and the ones that came into the grid as you walked,
// into "ringOut": a kGrid x kGrid picture laid out like the grid itself, "in
// a ring" (the column x, z is at [(x & 511) + (z & 511) * 512]), so walking
// never forces a full copy. Only the window around (centerX, centerZ),
// 256 blocks to each side, is valid. "areas" says what was updated (the world
// map stores exactly that). False if nothing changed. Thread-safe; only ONE
// user (the minimap): what it copies isn't copied again.
// "session" / "dimension": which start and dimension the copy belongs to.
bool CopyGrid(std::uint32_t knownVersion, std::uint32_t* ringOut, int& centerX, int& centerZ, std::uint32_t& version,
              std::vector<Area>& areas, std::uint32_t* session = nullptr, int* dimension = nullptr);

// How long the work took in the last frames (microseconds, average).
float CostUs();

}  // namespace luma::sdk::blocks
