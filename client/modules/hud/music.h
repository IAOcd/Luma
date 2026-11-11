// =============================================================================
//  music.h - Module "Now playing" (Luma's music viewer)
// =============================================================================
//  Shows the song playing on Windows (Spotify, the browser, Apple Music...)
//  on the HUD, and the "Halo" around the radial menu with its controls
//  (play/pause, next, previous, drag the ring to seek).
//
//  Luma started as a music player: this module is meant to look good and be
//  very configurable. Its settings live in render/music.h (so the radial and
//  the Music page can use them); this module saves and edits them.
//
//  It reads no game memory; what's playing comes from core/media.h, on its
//  own thread, and only while this module is on.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::hud {

class NowPlaying : public Module {
public:
    NowPlaying();
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
    unsigned int shownTrack_ = 0;  // to fade in a new song
    double changedAt_ = 0.0;
};

}  // namespace luma::modules::hud
