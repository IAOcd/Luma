// =============================================================================
//  debug_screen.h - Module "Debug screen" (F3)
// =============================================================================
//  Press F3 (or the key you choose) and, like Java Edition's F3, you get two
//  columns of exact information: the game on the left (position, block,
//  chunk, where you look, speed, the light where you stand, world or server,
//  ping, players, what you hold) and your PC on the right (Windows, CPU,
//  graphics card, screen, memory).
//
//  Nothing here gives an advantage: it's your own data and the world's light
//  at your feet. It only costs while it's open (the slow facts are cached).
//  Also the biome where you are (sdk/blocks) and, in your own worlds, the
//  seed (sdk/worldid). Not available yet: time of day and the block you
//  look at.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "modules/module.h"

namespace luma::modules::hud {

class DebugScreen : public Module {
public:
    DebugScreen();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool IsHudElement() const override { return false; }  // always in the corners
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    float size_ = 1.0f;        // text size
    bool background_ = true;   // a dark strip behind each line (like Java)
    bool hudColors_ = false;   // use the HUD's text color instead of white
    bool showPerformance_ = true;
    bool showPosition_ = true;
    bool showWorld_ = true;
    bool showSystem_ = true;
    // Refreshed once a second (not every frame).
    double nextSlow_ = 0.0;
    int players_ = 0;
    std::string world_;
};

}  // namespace luma::modules::hud
