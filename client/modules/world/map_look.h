// =============================================================================
//  map_look.h - How the maps look (the minimap and the world map share it)
// =============================================================================
//  Two kinds of settings:
//
//  1. HOW THE TERRAIN IS PAINTED (relief, water by depth, shores, texture):
//     applied when sdk/blocks paints the blocks. Changing them repaints what
//     the minimap has around you; the world map keeps the old look in the
//     places it remembered until you walk there again.
//
//  2. COLOR (brightness, contrast, saturation, the unexplored background):
//     applied when the picture is SHOWN, so everything changes at once. It's
//     a small table per color channel, built once when a setting changes:
//     cheap even for millions of pixels.
// =============================================================================
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>

namespace luma::modules::world::maplook {

struct Look {
    // 1. Painting the terrain
    float relief = 1.0f;     // how strong the light from the north-west is (0 = flat)
    bool heightTint = true;  // higher ground lighter, valleys darker
    bool waterDepth = true;  // water from light (shallow) to dark blue (deep)
    bool shores = true;      // a darker rim on land next to water
    bool texture = true;     // each block a tiny bit lighter or darker, like pixels of a texture
    bool biomes = true;      // grass, leaves and water in the color of their biome
    // 2. Color
    float brightness = 1.0f;  // 0.5 .. 1.5
    float contrast = 1.0f;    // 0.5 .. 1.5
    float saturation = 1.0f;  // 0 (gray) .. 2
    float background[3] = {0.086f, 0.078f, 0.071f};  // where nothing is known
};

Look& Get();
// Call after changing it (rebuilds the color table; the maps repaint).
void Changed();
std::uint32_t Version();

// A color (RGBA, R first) with brightness, contrast and saturation applied.
std::uint32_t Grade(std::uint32_t rgba);
// True if Grade changes nothing (the defaults): the maps skip it.
bool Plain();
// The unexplored background, as RGBA (R first).
std::uint32_t Background();

void Save(nlohmann::json& out);
void Load(const nlohmann::json& in);

}  // namespace luma::modules::world::maplook
