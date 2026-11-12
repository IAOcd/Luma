// =============================================================================
//  radar_style.h - How each kind of entity looks on the maps (part E)
// =============================================================================
//  Luma's own icons (not the game's mob faces): a pixel dot whose COLOR and
//  SHAPE say what it is, the same on the minimap and the world map:
//      players   white, round, the biggest
//      monsters  red diamond
//      animals   green;  water animals blue;  bats and the like gray
//      villagers amber square;  others (golems, boats...) lilac square
//      items     a small pale square
//  Higher than you: lighter. Lower: darker (so you can tell a mob in a cave
//  under you from one next to you).
// =============================================================================
#pragma once

#include <imgui.h>

#include <algorithm>

#include "sdk/radar.h"

namespace luma::modules::world::radarstyle {

struct Style {
    ImU32 color;
    int shape;  // 0 round, 1 diamond, 2 square
    int size;   // pixels
};

inline Style Of(sdk::radar::Kind kind) {
    using K = sdk::radar::Kind;
    switch (kind) {
        case K::Player: return {IM_COL32(255, 255, 255, 255), 0, 5};
        case K::Hostile: return {IM_COL32(236, 64, 52, 255), 1, 4};
        case K::Animal: return {IM_COL32(112, 206, 74, 255), 0, 4};
        case K::Water: return {IM_COL32(74, 160, 242, 255), 0, 4};
        case K::Ambient: return {IM_COL32(170, 170, 176, 255), 0, 3};
        case K::Villager: return {IM_COL32(238, 182, 74, 255), 2, 4};
        case K::Item: return {IM_COL32(236, 232, 196, 255), 2, 2};
        default: return {IM_COL32(194, 150, 232, 255), 2, 3};
    }
}

// Lighter when above you, darker when below (more than 3 blocks).
inline ImU32 ByHeight(ImU32 color, float dy) {
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    if (dy > 3.0f) {
        const float t = std::min(0.55f, (dy - 3.0f) * 0.05f + 0.25f);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x + (1 - c.x) * t, c.y + (1 - c.y) * t, c.z + (1 - c.z) * t, c.w));
    }
    if (dy < -3.0f) {
        const float t = std::max(0.4f, 1.0f - (-dy - 3.0f) * 0.04f - 0.2f);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * t, c.y * t, c.z * t, c.w));
    }
    return color;
}

}  // namespace luma::modules::world::radarstyle
