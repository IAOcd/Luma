// =============================================================================
//  minimap.cpp - Module "Minimap" (see minimap.h)
// =============================================================================
//  HOW THE PICTURE IS DRAWN: the blocks around you live in a 128 x 128
//  texture (one pixel per block, sdk/blocks.h). The map shows a piece of it:
//  a circle or a square around you, as big as the zoom says, turned if
//  "rotate" is on. It's drawn as a fan of triangles (a center and its edge);
//  each corner of a triangle says which point of the texture it shows
//  ("UV"). Since turning and zooming are straight-line math, the GPU fills
//  every pixel in between exactly. Your position is followed to the
//  fraction of a block, so the map glides instead of jumping block by block.
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

namespace {

constexpr float kPi = 3.14159265358979f;

ImU32 WithAlpha(ImU32 c, float a) {
    const ImU32 alpha = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

}  // namespace

Minimap::Minimap()
    : Module("minimap", "Minimap",
             "A map of the blocks around you (the chunks your game has loaded): zoom, rotation, round or square. "
             "Terrain only: no players or mobs.",
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
    const bool round = shape_ == 1;
    const ImVec2 c(o.x + side * 0.5f, o.y + side * 0.5f);
    const float radiusPx = side * 0.5f;

    // --- The picture from sdk/blocks: at most 5 times a second, and only if it changed ---
    const double now = rainbow::Seconds();
    if (now >= nextUpload_) {
        nextUpload_ = now + 0.2;
        if (pixels_.size() != static_cast<std::size_t>(kGrid) * kGrid) pixels_.assign(static_cast<std::size_t>(kGrid) * kGrid, 0);
        int cx = 0, cz = 0;
        std::uint32_t v = 0;
        if (sdk::blocks::CopyGrid(version_, pixels_.data(), cx, cz, v)) {
            version_ = v;
            textureX_ = cx;
            textureZ_ = cz;
            // Each block becomes 4 x 4 pixels: the GPU smooths textures when
            // it enlarges them, and a map of blocks should look crisp, like
            // pixel art. (512 x 512, uploaded at most 5 times a second.)
            constexpr int kUp = 4, kSide = kGrid * kUp;
            big_.resize(static_cast<std::size_t>(kSide) * kSide);
            for (int y = 0; y < kSide; ++y) {
                const std::uint32_t* src = pixels_.data() + static_cast<std::size_t>(y / kUp) * kGrid;
                std::uint32_t* dst = big_.data() + static_cast<std::size_t>(y) * kSide;
                for (int x = 0; x < kSide; ++x) dst[x] = src[x / kUp];
            }
            const auto* rgba = reinterpret_cast<const std::uint8_t*>(big_.data());
            if (texture_ == ImTextureID{}) {
                texture_ = render::gpu::CreateTexture(rgba, kSide, kSide);
            } else {
                render::gpu::UpdateTexture(texture_, rgba, kSide, kSide);
            }
        }
    }

    // --- Zoom (smooth) and where you are ---
    // How many blocks from the center to the edge. A turning square needs
    // its corners inside the 128 x 128 we know (so less).
    const float maxRadius = round ? 60.0f : (rotate_ ? 42.0f : 60.0f);
    const float target = std::clamp(zoom_, 12.0f, maxRadius);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    shownZoom_ = shownZoom_ <= 0.0f ? target : shownZoom_ + (target - shownZoom_) * std::min(1.0f, dt * 10.0f);
    sdk::player::Snapshot p;
    const bool havePlayer = sdk::player::Read(p);
    const float yaw = havePlayer ? p.yaw * kPi / 180.0f : 0.0f;
    // The game's yaw: 0 looks south (+Z), 90 west (-X).
    const ImVec2 forward(-std::sin(yaw), std::cos(yaw));  // world x, z
    const ImVec2 right(-std::cos(yaw), -std::sin(yaw));
    const float px = havePlayer ? p.feet[0] : static_cast<float>(textureX_) + 0.5f;
    const float pz = havePlayer ? p.feet[2] : static_cast<float>(textureZ_) + 0.5f;
    const float blocksPerPx = shownZoom_ / radiusPx;
    // A point of the map (pixels from the center) -> the world, and -> the texture.
    auto toWorld = [&](float dx, float dy) {
        if (rotate_) {
            // Up on the map = where you look.
            return ImVec2(px + (dx * right.x - dy * forward.x) * blocksPerPx, pz + (dx * right.y - dy * forward.y) * blocksPerPx);
        }
        return ImVec2(px + dx * blocksPerPx, pz + dy * blocksPerPx);  // north up: right = east (+X), down = south (+Z)
    };
    auto toUv = [&](ImVec2 w) {
        return ImVec2((w.x - static_cast<float>(textureX_ - kGrid / 2)) / kGrid, (w.y - static_cast<float>(textureZ_ - kGrid / 2)) / kGrid);
    };
    // And the other way: a direction of the world -> the map (for N, E, S, W).
    auto toScreenDir = [&](float wx, float wz) {
        if (rotate_) return ImVec2(wx * right.x + wz * right.y, -(wx * forward.x + wz * forward.y));
        return ImVec2(wx, wz);
    };

    // --- Background and the map itself ---
    const rh::Style& style = rh::CurrentStyle();
    const ImU32 back = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.backgroundColor[0], style.backgroundColor[1], style.backgroundColor[2], std::max(0.35f, style.background)));
    const float inner = radiusPx - 2.0f * u;  // the map, inside the frame
    if (round) {
        draw->AddCircleFilled(c, radiusPx, back, 64);
    } else {
        rh::Box(draw, m, o, ImVec2(o.x + side, o.y + side));
    }
    if (texture_ != ImTextureID{}) {
        // A fan: the center and the edge (a circle of 72 points, or the 4 corners).
        const int n = round ? 72 : 4;
        draw->PushTextureID(texture_);
        draw->PrimReserve(n * 3, n + 1);
        const ImDrawIdx base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        draw->PrimWriteVtx(c, toUv(toWorld(0.0f, 0.0f)), white);
        for (int i = 0; i < n; ++i) {
            float dx, dy;
            if (round) {
                const float a = static_cast<float>(i) / n * 2.0f * kPi;
                dx = std::cos(a) * inner;
                dy = std::sin(a) * inner;
            } else {
                dx = (i == 0 || i == 3) ? -inner : inner;
                dy = (i < 2) ? -inner : inner;
            }
            draw->PrimWriteVtx(ImVec2(c.x + dx, c.y + dy), toUv(toWorld(dx, dy)), white);
        }
        for (int i = 0; i < n; ++i) {
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % n));
        }
        draw->PopTextureID();
    }
    // The frame: a thin ring (or square line) in the HUD's text color.
    const ImU32 ring = WithAlpha(rh::TextColor(), 0.55f);
    if (round) {
        draw->AddCircle(c, inner + 0.5f * u, ring, 72, std::max(1.0f, u));
    } else {
        draw->AddRect(ImVec2(c.x - inner, c.y - inner), ImVec2(c.x + inner, c.y + inner), ring, 0.0f, 0, std::max(1.0f, u));
    }
    if (!sdk::blocks::Available()) {
        rh::CenteredText(draw, m, ImVec2(c.x - inner, c.y - 6.0f * u), ImVec2(c.x + inner, c.y + 6.0f * u), Tr("Reading the world..."),
                         rh::TextColor(0.8f));
    }

    // --- N, E, S, W on the edge (N red); with rotation they turn with the map ---
    if (cardinals_) {
        struct Cardinal {
            const char* letter;
            float x, z;
        };
        const Cardinal kCards[] = {{"N", 0, -1}, {"E", 1, 0}, {"S", 0, 1}, {"W", -1, 0}};
        for (const Cardinal& cd : kCards) {
            const ImVec2 d = toScreenDir(cd.x, cd.z);
            float edge = inner - 5.0f * u;
            if (!round) {
                // On a square, the letter sits on the side it points to.
                const float k = std::max(std::fabs(d.x), std::fabs(d.y));
                edge /= std::max(k, 0.001f);
            }
            const ImVec2 at(c.x + d.x * edge, c.y + d.y * edge);
            const float w = rh::TextSize(m, cd.letter).x;
            const bool north = cd.letter[0] == 'N';
            draw->AddCircleFilled(at, 4.6f * u, WithAlpha(back, 0.9f), 16);
            rh::Text(draw, m, ImVec2(at.x - w * 0.5f, at.y - m.capHeight * 0.5f - m.capTop), cd.letter,
                     north ? IM_COL32(240, 96, 80, 255) : rh::TextColor(0.85f));
        }
    }

    // --- You: an arrow in the middle ---
    if (havePlayer) {
        const ImVec2 dir = rotate_ ? ImVec2(0.0f, -1.0f) : ImVec2(forward.x, forward.y);
        const ImVec2 side2(-dir.y, dir.x);
        const float r = 4.5f * u;
        const ImVec2 tip(c.x + dir.x * r, c.y + dir.y * r);
        const ImVec2 left(c.x - dir.x * r * 0.7f + side2.x * r * 0.75f, c.y - dir.y * r * 0.7f + side2.y * r * 0.75f);
        const ImVec2 rightPt(c.x - dir.x * r * 0.7f - side2.x * r * 0.75f, c.y - dir.y * r * 0.7f - side2.y * r * 0.75f);
        const ImVec2 notch(c.x - dir.x * r * 0.25f, c.y - dir.y * r * 0.25f);
        draw->AddTriangleFilled(tip, left, notch, IM_COL32(20, 20, 24, 230));
        draw->AddTriangleFilled(tip, notch, rightPt, IM_COL32(20, 20, 24, 230));
        const float k = 0.62f;
        auto in = [&](ImVec2 q) { return ImVec2(c.x + (q.x - c.x) * k, c.y + (q.y - c.y) * k); };
        draw->AddTriangleFilled(in(tip), in(left), in(notch), IM_COL32(255, 255, 255, 255));
        draw->AddTriangleFilled(in(tip), in(notch), in(rightPt), IM_COL32(220, 220, 220, 255));
    }

    // --- Under the map: coordinates and (optional) the cost ---
    float height = side;
    float y = o.y + side + 2.0f * u;
    auto lineUnder = [&](const char* text, ImU32 color) {
        const float w = rh::TextSize(m, text).x;
        rh::Text(draw, m, ImVec2(c.x - w * 0.5f, y - m.capTop), text, color);
        y += m.capHeight + 3.0f * u;
        height += m.capHeight + 3.0f * u;
    };
    if (coords_ && havePlayer) {
        char text[64];
        std::snprintf(text, sizeof(text), "%d, %d, %d", static_cast<int>(std::floor(p.feet[0])), static_cast<int>(std::floor(p.feet[1])),
                      static_cast<int>(std::floor(p.feet[2])));
        lineUnder(text, rh::TextColor(0.9f));
    }
    if (showCost_) {
        char cost[48];
        std::snprintf(cost, sizeof(cost), "%.0f us", sdk::blocks::CostUs());
        lineUnder(cost, rh::TextColor(0.6f));
    }
    return ImVec2(side, height);
}

bool Minimap::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Shape", &shape_, {"Square", "Round"});
    changed |= ui::Toggle("Rotate with you", &rotate_, "(up = where you look; off = north up)");
    changed |= ui::Slider("Zoom (blocks to the edge)", &zoom_, 12.0f, 60.0f, "%.0f");
    changed |= ui::Slider("Size", &size_, 60.0f, 220.0f, "%.0f");
    changed |= ui::Toggle("N, E, S, W", &cardinals_);
    changed |= ui::Toggle("Coordinates under it", &coords_);
    changed |= ui::Toggle("Show its cost", &showCost_, "(microseconds per frame)");
    ui::Hint("Move it and scale it with Edit HUD. It reads the blocks a little every frame on the game's own drawing "
             "thread, and only rereads what's new or old.");
    return changed;
}

void Minimap::SaveSettings(nlohmann::json& out) const {
    out["size"] = size_;
    out["cost"] = showCost_;
    out["shape"] = shape_;
    out["rotate"] = rotate_;
    out["zoom"] = zoom_;
    out["cardinals"] = cardinals_;
    out["coords"] = coords_;
}

void Minimap::LoadSettings(const nlohmann::json& in) {
    size_ = std::clamp(in.value("size", 110.0f), 60.0f, 220.0f);
    showCost_ = in.value("cost", false);
    shape_ = std::clamp(in.value("shape", 1), 0, 1);
    rotate_ = in.value("rotate", false);
    zoom_ = std::clamp(in.value("zoom", 40.0f), 12.0f, 60.0f);
    cardinals_ = in.value("cardinals", true);
    coords_ = in.value("coords", false);
}

}  // namespace luma::modules::world
