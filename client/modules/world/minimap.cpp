// =============================================================================
//  minimap.cpp - Module "Minimap" (see minimap.h)
// =============================================================================
#include "modules/world/minimap.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/tr.h"
#include "render/gpu.h"
#include "render/hud.h"
#include "render/icons.h"
#include "render/rainbow.h"
#include "render/ui.h"
#include "sdk/blocks.h"
#include "sdk/player.h"

namespace luma::modules::world {

namespace rh = render::hud;
namespace ui = render::ui;

Minimap::Minimap()
    : Module("minimap", "Minimap",
             "A map of the blocks around you (the chunks your game has loaded), north at the top. First test version: "
             "rotation, zoom and waypoints are coming.",
             Category::World) {
    HudLayout l;
    l.x = 0.996f;
    l.y = 0.16f;
    l.anchorX = 1.0f;
    SetDefaultLayout(l);
}

const char* Minimap::IconSvg() const {
    return render::icons::kMapPin;
}

void Minimap::OnEnable() {
    sdk::blocks::Want(true);
    version_ = 0;
}

void Minimap::OnDisable() {
    sdk::blocks::Want(false);
}

ImVec2 Minimap::OnRenderHud(const HudFrame& frame) {
    constexpr int kGrid = sdk::blocks::kGrid;
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float side = std::floor(size_ * u);
    const ImVec2 o = frame.pos;
    ImDrawList* draw = frame.draw;
    rh::Box(draw, m, o, ImVec2(o.x + side, o.y + side));

    // The picture: copied from sdk/blocks at most 5 times a second, and
    // uploaded to the GPU only if it changed (the same texture is reused).
    const double now = rainbow::Seconds();
    if (now >= nextUpload_) {
        nextUpload_ = now + 0.2;
        if (pixels_.size() != static_cast<std::size_t>(kGrid) * kGrid) pixels_.assign(static_cast<std::size_t>(kGrid) * kGrid, 0);
        int cx = 0, cz = 0;
        std::uint32_t v = 0;
        if (sdk::blocks::CopyGrid(version_, pixels_.data(), cx, cz, v)) {
            version_ = v;
            const auto* rgba = reinterpret_cast<const std::uint8_t*>(pixels_.data());
            if (texture_ == ImTextureID{}) {
                texture_ = render::gpu::CreateTexture(rgba, kGrid, kGrid);
            } else {
                render::gpu::UpdateTexture(texture_, rgba, kGrid, kGrid);
            }
        }
    }
    const float pad = 2.0f * u;
    const ImVec2 mapMin(o.x + pad, o.y + pad), mapMax(o.x + side - pad, o.y + side - pad);
    if (texture_ != ImTextureID{}) {
        draw->AddImage(texture_, mapMin, mapMax);
    }
    if (!sdk::blocks::Available()) {
        rh::Text(draw, m, ImVec2(mapMin.x + 3.0f * u, mapMin.y + 3.0f * u), Tr("Reading the world..."), rh::TextColor(0.8f));
    }

    // You: an arrow in the middle, pointing where you look.
    sdk::player::Snapshot p;
    if (sdk::player::Read(p)) {
        const float yaw = p.yaw * 3.14159265f / 180.0f;
        // The game's yaw: 0 looks south (+Z, down on the map), 90 west (-X, left).
        const ImVec2 dir(-std::sin(yaw), std::cos(yaw));
        const ImVec2 c((mapMin.x + mapMax.x) * 0.5f, (mapMin.y + mapMax.y) * 0.5f);
        const float r = 4.0f * u;
        const ImVec2 tip(c.x + dir.x * r, c.y + dir.y * r);
        const ImVec2 left(c.x - dir.y * r * 0.6f - dir.x * r * 0.6f, c.y + dir.x * r * 0.6f - dir.y * r * 0.6f);
        const ImVec2 right(c.x + dir.y * r * 0.6f - dir.x * r * 0.6f, c.y - dir.x * r * 0.6f - dir.y * r * 0.6f);
        draw->AddTriangleFilled(tip, left, right, IM_COL32(20, 20, 24, 230));
        const float k = 0.7f;
        draw->AddTriangleFilled(ImVec2(c.x + (tip.x - c.x) * k, c.y + (tip.y - c.y) * k),
                                ImVec2(c.x + (left.x - c.x) * k, c.y + (left.y - c.y) * k),
                                ImVec2(c.x + (right.x - c.x) * k, c.y + (right.y - c.y) * k), rh::TextColor());
    }
    float height = side;
    if (showCost_) {
        char cost[48];
        std::snprintf(cost, sizeof(cost), "%.0f us", sdk::blocks::CostUs());
        rh::Text(draw, m, ImVec2(o.x, o.y + side + 2.0f * u - m.capTop), cost, rh::TextColor(0.7f));
        height += m.capHeight + 4.0f * u;
    }
    return ImVec2(side, height);
}

bool Minimap::DrawSettings() {
    bool changed = false;
    changed |= ui::Slider("Size", &size_, 60.0f, 220.0f, "%.0f");
    changed |= ui::Toggle("Show its cost", &showCost_, "(microseconds per frame: this is a test version)");
    ui::Hint("It reads the blocks a little every frame, on the game's own drawing thread. The whole map refreshes "
             "every one or two seconds.");
    return changed;
}

void Minimap::SaveSettings(nlohmann::json& out) const {
    out["size"] = size_;
    out["showCost"] = showCost_;
}

void Minimap::LoadSettings(const nlohmann::json& in) {
    size_ = std::clamp(in.value("size", 110.0f), 60.0f, 220.0f);
    showCost_ = in.value("showCost", true);
}

}  // namespace luma::modules::world
