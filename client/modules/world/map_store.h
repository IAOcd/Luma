// =============================================================================
//  map_store.h - What you explored, kept for the world map (part D)
// =============================================================================
//  The minimap only knows the 128 x 128 blocks around you. The world map
//  needs to REMEMBER everything you walked through, so every picture the
//  minimap makes is also copied here, block by block (already colored and
//  shaded, exactly as the minimap shows it).
//
//  REGIONS: the world is cut in squares of 256 x 256 blocks. Each one is
//  256 x 256 colors (4 bytes each: 256 KB) in memory, and a file on disk:
//      %APPDATA%\Luma\worldmap\<world>\dim<0|1|2>\r.<x>.<z>.bin
//  Only the regions you're near or looking at are in memory (at most 256;
//  the oldest go back to disk). Changed ones are saved a few seconds later,
//  one per tick, so saving never stops a frame.
//
//  FILE FORMAT (own, simple): "LMR1", then runs of the same color: a count
//  (2 bytes) and a color (4 bytes). Unexplored areas are long runs of 0, so
//  a half-explored region is small.
//
//  Cave mode pictures are NOT stored (only the surface, for now).
//
//  Threads: used from the render thread and Luma's tick, both with the
//  state lock held: no lock of its own.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace luma::modules::world::mapstore {

inline constexpr int kRegion = 256;  // blocks per side of a region

// Which world and dimension (from waypoints::StorageName and the dimension).
// Changing it saves and forgets the previous one. "" = none.
void SetWorld(const std::string& storageName, int dimension);

// A picture of the minimap: kSide x kSide colors (RGBA, R first; 0 =
// unknown), row by row from the north-west corner at (x0, z0).
void Merge(const std::uint32_t* pixels, int side, int x0, int z0);
// The same from the minimap's "ring" picture (see sdk/blocks.h): the blocks
// x0 <= x < x1, z0 <= z < z1, each at [(x & mask) + (z & mask) * side].
void MergeRing(const std::uint32_t* ring, int side, int x0, int z0, int x1, int z1);

// The color of one block (0 = not explored or not loaded). "load": read the
// region from disk if it isn't in memory (the world map does; it's slow).
std::uint32_t At(int x, int z, bool load);

// One row of a region in memory (256 colors from its west edge), or nullptr
// if that region isn't in memory. "rz"/"rx" are region numbers; "z" the
// world z of the row. Fast: for drawing many blocks.
const std::uint32_t* Row(int rx, int rz, int z);
// The region a block column is in (rounds down, also for negatives).
inline int RegionOf(int v) { return v >= 0 ? v / kRegion : -((-v - 1) / kRegion) - 1; }

// Prepare the regions of an area (load them from disk, a few per call).
// True when they're all ready.
bool Prefetch(int x0, int z0, int x1, int z1, int maxLoads);

// Counts something changed (the world map redraws when it changes).
std::uint32_t Version();

// Saves one changed region (call often; cheap when nothing changed).
void Tick(double now);
// Saves all (when leaving or unloading).
void Flush();

}  // namespace luma::modules::world::mapstore
