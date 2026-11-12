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
#include "sdk/blocks.h"
#include "sdk/radar.h"

namespace luma::render::pixel {
struct Form;
}

namespace luma::modules::world {

class Minimap : public Module {
public:
    Minimap();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasTick() const override { return true; }
    void OnTick() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool DrawWaypointSettings();
    // Its page in the world map's sidebar (everything to customize it).
    bool DrawMapPanel(render::pixel::Form& form);
    static bool MapPanel(render::pixel::Form& form);
    void DrawWorldWaypoints();
    float size_ = 110.0f;     // HUD pixels per side
    int shape_ = 1;           // 0 square, 1 round
    bool rotate_ = false;     // up = where you look (off = north up)
    float zoom_ = 40.0f;      // blocks from the center to the edge
    float shownZoom_ = 0.0f;  // (animated toward zoom_)
    bool cardinals_ = true;   // N, E, S, W on the edge
    bool coords_ = false;     // your coordinates under the map
    int caveMode_ = 0;        // 0 auto (a roof over you), 1 off, 2 always
    // The look of the minimap itself (set up in the world map's sidebar)
    int frameMode_ = 1;               // the ring around it: 0 accent, 1 text color, 2 own color, 3 none
    float frameColor_[3] = {1.0f, 1.0f, 1.0f};
    int frameWidth_ = 1;              // HUD pixels
    float backOpacity_ = 0.6f;        // the background behind the map
    float mapOpacity_ = 1.0f;         // the map itself
    float arrowColor_[3] = {0.98f, 1.0f, 1.0f};  // you
    bool northRed_ = true;            // the N in red
    // The entity radar (local worlds only: sdk/radar.h)
    bool radar_ = true;
    bool radarPlayers_ = true, radarHostile_ = true, radarAnimals_ = true, radarOthers_ = true, radarItems_ = false;
    int radarSize_ = 0;               // +0..2 pixels
    bool radarHeight_ = true;         // lighter above you, darker below
    bool waypoints_ = true;   // show the waypoints (modules/world/waypoints.h)
    bool waypointNames_ = true;  // and the name of the nearest ones
    int addKey_ = 'B';        // the key that saves a waypoint where you are
    bool keyWasDown_ = false;
    int mapKey_ = 'M';        // the key that opens the world map
    bool mapKeyWasDown_ = false;
    bool beams_ = true;        // a column of light at each waypoint, in the world
    bool worldLabels_ = true;  // its banner, name and distance, in the world
    bool deathPoint_ = true;   // a waypoint where you died
    bool beamsSent_ = false;
    unsigned deathFrames_ = 0xFFFFFFFFu;
    double lastDeath_ = -100.0;
    float alive_[3] = {0, 0, 0};  // the last place you stood alive (while playing)
    double releasedAt_ = -1.0;    // when the game let go of the mouse (-1 = it has it)
    unsigned menusSeen_ = 0;
    bool hiddenScreen_ = false;
    int aliveDim_ = -1;
    std::uint32_t hiddenWorld_ = 0;  // (which world it was when the mouse was freed)   // the mouse is free and no known screen is drawn
    bool showCost_ = false;   // how many microseconds it costs per frame
    int textureX_ = 0, textureZ_ = 0;  // the world column at the texture's center
    ImTextureID texture_{};
    std::uint32_t version_ = 0;
    double nextUpload_ = 0.0;
    std::vector<std::uint32_t> pixels_;
    std::vector<sdk::blocks::Area> areas_;  // (what the last copy updated)
    std::vector<sdk::radar::Entity> entities_;  // (kept between frames: no new memory each frame)
    std::vector<std::uint32_t> view_;  // the map's own picture (512 x 512): the square it shows
    int viewX_ = 0, viewZ_ = 0;        // its center (world block)
    float viewR_ = 0.0f;               // blocks from its center to its edge
    std::uint32_t lookVersion_ = 0;    // (repaint when the look changes)
    std::uint32_t session_ = 0, worldSession_ = 0;  // (sdk/blocks' new starts)
};

}  // namespace luma::modules::world
