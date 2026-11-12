// =============================================================================
//  minimap.h - Module "Minimap" (Phase 11, part 1: the first test)
// =============================================================================
//  A square map of the 128 x 128 blocks around you, north at the top, with an
//  arrow where you are and where you look. The blocks are read by
//  sdk/blocks.h (on the game's own drawing thread, a little each frame) and
//  painted with a color per kind of block, darker on slopes like the game's
//  paper maps.
//
//  Round or square, zoom, rotation with your view, N/E/S/W and coordinates.
//  Waypoints come in the next part. It shows terrain only: the chunks
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
    int shape_ = 1;           // 0 square, 1 round
    bool rotate_ = false;     // up = where you look (off = north up)
    float zoom_ = 40.0f;      // blocks from the center to the edge
    float shownZoom_ = 0.0f;  // (animated toward zoom_)
    bool cardinals_ = true;   // N, E, S, W on the edge
    bool coords_ = false;     // your coordinates under the map
    int caveMode_ = 0;        // 0 auto (a roof over you), 1 off, 2 always
    bool showCost_ = false;   // how many microseconds it costs per frame
    int textureX_ = 0, textureZ_ = 0;  // the world column at the texture's center
    ImTextureID texture_{};
    std::uint32_t version_ = 0;
    double nextUpload_ = 0.0;
    std::vector<std::uint32_t> pixels_;
    std::vector<std::uint32_t> big_;  // the same, 4x bigger (crisp blocks)
};

}  // namespace luma::modules::world
