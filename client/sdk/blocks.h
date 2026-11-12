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

namespace luma::sdk::blocks {

// Who wants blocks read (the work only happens while someone does).
void Want(bool want);
bool Wanted();

// The block names were found (the minimap can work).
bool Available();

// Called by sdk/draw3d every frame, on the game's drawing thread, with the
// game's BlockSource. Does this frame's share of the minimap's work.
void OnRegion(void* region);

// Cave mode (see blocks.cpp): 0 = automatic (when there's a roof over you),
// 1 = never, 2 = always. CaveActive(): what the grid shows now.
void SetCaveMode(int mode);
bool CaveActive();

// --- The minimap's grid (filled by OnRegion) --------------------------------------
inline constexpr int kGrid = 128;  // blocks per side

struct Cell {
    std::uint32_t rgba = 0;  // 0 = not known yet (unloaded chunk or not scanned)
    std::int16_t height = 0;
};

// Copies the grid around (centerX, centerZ) (row by row, north at the top),
// if it changed since "knownVersion". Thread-safe.
bool CopyGrid(std::uint32_t knownVersion, std::uint32_t* rgbaOut, int& centerX, int& centerZ, std::uint32_t& version);

// How long the work took in the last frames (microseconds, average).
float CostUs();

}  // namespace luma::sdk::blocks
