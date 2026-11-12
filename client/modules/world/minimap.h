// =============================================================================
//  minimap.h - Module "Minimap" (Phase 11, part 1: the first test)
// =============================================================================
//  A square map of the 128 x 128 blocks around you, north at the top, with an
//  arrow where you are and where you look. The blocks are read by
//  sdk/blocks.h (on the game's own drawing thread, a little each frame) and
//  painted with a color per kind of block, darker on slopes like the game's
//  paper maps.
//
//  This first version is the test: does reading the world work, and what
//  does it cost? (The cost shows under the map.) Rotation, zoom, round shape
//  and waypoints come in the next parts. It shows terrain only: the chunks
//  your game already has loaded. (No players or mobs: an entity radar will
//  only work in local worlds.)
// =============================================================================
#pragma once

#include <imgui.h>

#include <cstdint>
#include <vector>

#include "modules/module.h"

namespace luma::modules::world {

class Minimap : public Module {
public:
    Minimap();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    float size_ = 110.0f;     // HUD pixels per side
    bool showCost_ = true;    // the test: how many microseconds it costs
    ImTextureID texture_{};
    std::uint32_t version_ = 0;
    double nextUpload_ = 0.0;
    std::vector<std::uint32_t> pixels_;
};

}  // namespace luma::modules::world
