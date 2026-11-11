// =============================================================================
//  dynamic_lights.h - Module "Dynamic lights"
// =============================================================================
//  Holding a light-giving item (torch, lantern, glowstone, lava bucket...)
//  lights the world around you, on your screen only, in local worlds and on
//  servers. Each item gives the same light it gives when placed.
//  The game-memory part (how the light reaches the game) is in sdk/dynlight.h.
//
//  PERFORMANCE: every time the light moves one block, the game rebuilds the
//  pieces of the world it touches (like when a block changes). That's capped
//  by "Updates per second". Standing still costs nothing extra.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::visual {

class DynamicLights : public Module {
public:
    DynamicLights();
    const char* IconSvg() const override;
    bool NeedsPlayer() const override { return true; }
    bool SdkReady() const override;
    bool HasTick() const override { return true; }
    void OnTick() override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

    // The light an item gives (0 = none). "minecraft:torch" -> 14.
    static int LightOf(const char* itemId);

private:
    bool offhand_ = true;    // the item in the other hand counts too
    bool entities_ = true;   // light your player, mobs and your hand, not only blocks
    int updates_ = 10;       // rebuilds per second at most while moving
};

}  // namespace luma::modules::visual
