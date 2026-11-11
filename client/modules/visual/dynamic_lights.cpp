// =============================================================================
//  dynamic_lights.cpp - Module "Dynamic lights" (see dynamic_lights.h)
// =============================================================================
#include "modules/visual/dynamic_lights.h"

#include "render/icons.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/ui.h"
#include "sdk/dynlight.h"
#include "sdk/inventory.h"
#include "sdk/player.h"

namespace luma::modules::visual {

namespace ui = render::ui;
namespace inv = sdk::inventory;

namespace {

struct ItemLight {
    const char* id;  // without "minecraft:"
    int level;
};

// The light each item gives when placed as a block (Bedrock values).
constexpr ItemLight kLights[] = {
    {"torch", 14},
    {"soul_torch", 10},
    {"redstone_torch", 7},
    {"copper_torch", 14},
    {"lantern", 15},
    {"soul_lantern", 10},
    {"glowstone", 15},
    {"sea_lantern", 15},
    {"shroomlight", 15},
    {"lit_pumpkin", 15},          // jack o'lantern
    {"beacon", 15},
    {"conduit", 15},
    {"end_rod", 14},
    {"campfire", 15},
    {"soul_campfire", 10},
    {"lava_bucket", 15},
    {"ochre_froglight", 15},
    {"verdant_froglight", 15},
    {"pearlescent_froglight", 15},
    {"glow_berries", 14},
    {"crying_obsidian", 10},
    {"magma", 3},                 // magma block
    {"sea_pickle", 6},
    {"glow_lichen", 7},
    {"amethyst_cluster", 5},
    {"large_amethyst_bud", 4},
    {"medium_amethyst_bud", 2},
    {"small_amethyst_bud", 1},
    {"candle", 3},
    {"blaze_rod", 10},            // not a block, but it glows (like in Java mods)
    {"blaze_powder", 8},
    {"nether_star", 12},
    {"glow_ink_sac", 4},
    {"lit_redstone_lamp", 15},
    {"redstone_lamp", 15},
    {"respawn_anchor", 15},
    {"enchanting_table", 7},
    {"ender_chest", 7},
    {"brewing_stand", 1},
    {"dragon_egg", 1},
    {"sculk_catalyst", 6},
    {"light_block", 15},
};

int HeldLight(bool offhand) {
    int best = 0;
    inv::Item item;
    const int slot = inv::SelectedSlot();
    if (slot >= 0 && inv::ReadSlot(slot, item) && item.count > 0) {
        best = DynamicLights::LightOf(item.id);
    }
    if (offhand && inv::ReadOffhand(item) && item.count > 0) {
        best = std::max(best, DynamicLights::LightOf(item.id));
    }
    return best;
}

}  // namespace

DynamicLights::DynamicLights()
    : Module("dynamic_lights", "Dynamic lights",
             "A torch, lantern, glowstone or any light-giving item in your hand lights up the world around you. "
             "Only on your screen: the server and mob spawning see the real light.",
             Category::Visual) {}

const char* DynamicLights::IconSvg() const {
    return render::icons::kFlame;
}

bool DynamicLights::SdkReady() const {
    return sdk::dynlight::Available();
}

int DynamicLights::LightOf(const char* itemId) {
    if (itemId == nullptr) {
        return 0;
    }
    const char* name = std::strncmp(itemId, "minecraft:", 10) == 0 ? itemId + 10 : itemId;
    for (const ItemLight& light : kLights) {
        if (std::strcmp(name, light.id) == 0) {
            return light.level;
        }
    }
    // All the colored candles ("red_candle"...) give the same as a candle.
    const std::size_t len = std::strlen(name);
    if (len > 7 && std::strcmp(name + len - 7, "_candle") == 0) {
        return 3;
    }
    return 0;
}

void DynamicLights::OnEnable() {
    sdk::dynlight::SetLightEntities(entities_);
    sdk::dynlight::SetMaxRebuildsPerSecond(updates_);
}

void DynamicLights::OnDisable() {
    sdk::dynlight::SetLight(0, 0, 0, 0);  // removes the light (its pieces get rebuilt)
}

void DynamicLights::OnTick() {
    sdk::player::Snapshot p;
    if (!sdk::player::Read(p)) {
        sdk::dynlight::SetLight(0, 0, 0, 0);
        return;
    }
    // The light is at hand height: one block above the feet.
    const int x = static_cast<int>(std::floor(p.feet[0]));
    const int y = static_cast<int>(std::floor(p.feet[1])) + 1;
    const int z = static_cast<int>(std::floor(p.feet[2]));
    sdk::dynlight::SetLight(x, y, z, HeldLight(offhand_));
}

bool DynamicLights::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Other hand too", &offhand_);
    if (ui::Toggle("Light entities and your hand", &entities_)) {
        sdk::dynlight::SetLightEntities(entities_);
        changed = true;
    }
    if (ui::SliderInt("Updates per second", &updates_, 2, 20, "%d")) {
        sdk::dynlight::SetMaxRebuildsPerSecond(updates_);
        changed = true;
    }
    ui::Hint("Each update rebuilds the pieces of the world around the light, like when a block changes. Fewer "
             "updates = lighter, but the light follows you in small jumps.");
    return changed;
}

void DynamicLights::SaveSettings(nlohmann::json& out) const {
    out["offhand"] = offhand_;
    out["entities"] = entities_;
    out["updates"] = updates_;
}

void DynamicLights::LoadSettings(const nlohmann::json& in) {
    offhand_ = in.value("offhand", true);
    entities_ = in.value("entities", true);
    updates_ = std::clamp(in.value("updates", 10), 2, 20);
    if (IsEnabled()) {
        OnEnable();
    }
}

}  // namespace luma::modules::visual
