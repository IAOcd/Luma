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

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "core/tr.h"
#include "render/gpu.h"
#include "render/hud.h"
#include "render/notifications.h"
#include "render/icons.h"
#include "render/rainbow.h"
#include "render/ui.h"
#include "modules/world/waypoints.h"
#include "sdk/blocks.h"
#include "sdk/mouse_grab.h"
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
    waypoints::Flush();
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
    sdk::blocks::SetCaveMode(caveMode_);
    waypoints::Update(now, sdk::blocks::Dimension());
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

    // --- Waypoints: a dot on the map, or an arrow on its edge when far ---
    // The other way of toWorld: a point of the world -> pixels from the center.
    auto toScreen = [&](float wx, float wz) {
        const float dx = wx - px, dz = wz - pz;
        if (rotate_) return ImVec2((dx * right.x + dz * right.y) / blocksPerPx, -(dx * forward.x + dz * forward.y) / blocksPerPx);
        return ImVec2(dx / blocksPerPx, dz / blocksPerPx);
    };
    if (waypoints_ && havePlayer) {
        // THE LOOK, made of Minecraft pixels: a little banner (a square with
        // a stepped point at the bottom) whose tip touches the place. Its top
        // row is lighter and its bottom darker, like the game's buttons, and
        // it casts a hard shadow one pixel down-right, like the game's text.
        // Inside, the initial in the game's own font. Far away, the same
        // banner waits on the map's edge, in the place's direction.
        // Everything is drawn on a grid of "q" screen pixels (one pixel of
        // the HUD), so nothing looks blurry.
        const int qi = std::clamp(static_cast<int>(std::lround(u)), 1, 8);
        const float q = static_cast<float>(qi);
        ImFont* pixel = render::hud::PixelFontAt(qi);
        ImFont* font = pixel != nullptr ? pixel : render::hud::SmoothFont();
        const float fontSize = pixel != nullptr ? 11.0f * q : 9.0f * q;
        const float capTop = pixel != nullptr ? 2.0f * q : 0.2f * fontSize;
        const int dim = waypoints::Dimension();
        auto shade = [](const float* rgb, float k, int alpha) {
            return IM_COL32(static_cast<int>(std::clamp(rgb[0] * k, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(rgb[1] * k, 0.0f, 1.0f) * 255.0f),
                            static_cast<int>(std::clamp(rgb[2] * k, 0.0f, 1.0f) * 255.0f), alpha);
        };
        const float reach = inner - 6.0f * q;
        // FAR AWAY: a pixel-art arrow on the edge, pointing at the place. It's
        // drawn cell by cell: each cell of a 13 x 13 grid around the point is
        // painted if its center falls inside the arrow's shape turned toward
        // the place (a dark outline around, the lower half a bit darker). So
        // it stays made of square pixels at any angle.
        auto inArrow = [](float x, float y, float grow) {
            // The shape pointing right (+x): a tip and a notch at the back.
            const float tx = 3.6f * grow, bx = -3.2f * grow, by = 3.2f * grow, nx = -1.0f * grow;
            auto inTri = [](float px0, float py0, float ax, float ay, float bx0, float by0, float cx0, float cy0) {
                const float d1 = (px0 - bx0) * (ay - by0) - (ax - bx0) * (py0 - by0);
                const float d2 = (px0 - cx0) * (by0 - cy0) - (bx0 - cx0) * (py0 - cy0);
                const float d3 = (px0 - ax) * (cy0 - ay) - (cx0 - ax) * (py0 - ay);
                const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                return !(neg && pos);
            };
            return inTri(x, y, tx, 0.0f, bx, by, nx, 0.0f) || inTri(x, y, tx, 0.0f, nx, 0.0f, bx, -by);
        };
        auto arrow = [&](ImVec2 center, ImVec2 dir, const float* rgb) {
            const ImVec2 cell0(std::floor(center.x), std::floor(center.y));
            for (int gy = -6; gy <= 6; ++gy) {
                for (int gx = -6; gx <= 6; ++gx) {
                    // This cell's center, turned into the arrow's own frame.
                    const float x = static_cast<float>(gx), y = static_cast<float>(gy);
                    const float lx = x * dir.x + y * dir.y, ly = -x * dir.y + y * dir.x;
                    ImU32 color = 0;
                    if (inArrow(lx, ly, 1.0f)) {
                        color = ly > 0.0f ? shade(rgb, 0.72f, 255) : shade(rgb, 1.0f, 255);
                    } else if (inArrow(lx, ly, 1.5f)) {
                        color = IM_COL32(18, 18, 22, 230);
                    }
                    if (color == 0) continue;
                    const ImVec2 a(cell0.x + static_cast<float>(gx) * q, cell0.y + static_cast<float>(gy) * q);
                    draw->AddRectFilled(a, ImVec2(a.x + q, a.y + q), color);
                }
            }
        };
        // One pixel-art rectangle: x0..x1, y0..y1 in HUD pixels from "o".
        auto pixelRect = [&](ImVec2 o, float x0, float y0, float x1, float y1, ImU32 color) {
            draw->AddRectFilled(ImVec2(o.x + x0 * q, o.y + y0 * q), ImVec2(o.x + x1 * q, o.y + y1 * q), color);
        };
        // The banner, its tip at "o": a 7 x 9 body (just room for a letter
        // and one pixel around) and a point of 2 steps.
        auto banner = [&](ImVec2 o, ImU32 body, ImU32 top, ImU32 bottom) {
            pixelRect(o, -3, -11, 4, -2, body);
            pixelRect(o, -3, -11, 4, -10, top);
            pixelRect(o, -3, -3, 4, -2, bottom);
            pixelRect(o, -2, -2, 3, -1, bottom);
            pixelRect(o, -1, -1, 2, 0, bottom);
        };
        int named = 0;
        for (const waypoints::Waypoint& w : waypoints::List()) {
            if (!w.visible || (dim >= 0 && w.dimension != dim)) continue;
            ImVec2 d = toScreen(static_cast<float>(w.x) + 0.5f, static_cast<float>(w.z) + 0.5f);
            const float k = round ? std::sqrt(d.x * d.x + d.y * d.y) : std::max(std::fabs(d.x), std::fabs(d.y));
            const bool isFar = k > reach;
            if (isFar) {
                d.x *= reach / k;
                d.y *= reach / k;
                const float len = std::max(0.001f, std::sqrt(d.x * d.x + d.y * d.y));
                arrow(ImVec2(c.x + d.x, c.y + d.y), ImVec2(d.x / len, d.y / len), w.color);
                continue;
            }
            // The tip, on the pixel grid (and, at the edge, the banner centered on the point).
            ImVec2 tip(std::floor(c.x + d.x), std::floor(c.y + d.y));
            const float alpha = isFar ? 0.85f : 1.0f;
            const int a8 = static_cast<int>(alpha * 255.0f);
            banner(ImVec2(tip.x + q, tip.y + q), IM_COL32(0, 0, 0, static_cast<int>(alpha * 110.0f)), IM_COL32(0, 0, 0, static_cast<int>(alpha * 110.0f)),
                   IM_COL32(0, 0, 0, static_cast<int>(alpha * 110.0f)));  // the shadow
            banner(tip, shade(w.color, 1.0f, a8), shade(w.color, 1.3f, a8), shade(w.color, 0.68f, a8));
            // The initial: white with the game's text shadow; dark on light colors.
            char initial[8] = {0};
            const unsigned char first = static_cast<unsigned char>(w.name[0]);
            if (first >= 0x80) {
                const int n = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : 2;
                for (int i = 0; i < n && w.name[i] != 0; ++i) initial[i] = w.name[i];
            } else {
                initial[0] = first != 0 ? static_cast<char>(std::toupper(first)) : '?';
            }
            const float lum = 0.299f * w.color[0] + 0.587f * w.color[1] + 0.114f * w.color[2];
            const ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, initial);
            // (the pixel font leaves one pixel of space after each letter)
            const float lw = pixel != nullptr ? ts.x - q : ts.x;
            const ImVec2 tp(std::floor(tip.x + 0.5f * q - lw * 0.5f), tip.y - 10.0f * q - capTop);
            if (lum > 0.7f) {
                draw->AddText(font, fontSize, tp, IM_COL32(40, 40, 40, a8), initial);
            } else {
                draw->AddText(font, fontSize, ImVec2(tp.x + q, tp.y + q), shade(w.color, 0.25f, a8), initial);
                draw->AddText(font, fontSize, tp, IM_COL32(255, 255, 255, a8), initial);
            }
            // The name, like the game's name tags: white letters on a dark
            // see-through band (only on the map, not on the edge; at most 6).
            if (waypointNames_ && !isFar && named < 6 && w.name[0] != 0) {
                ++named;
                // (one pixel size smaller than the banner: the name helps, the map matters more)
                const int nq = std::max(1, qi - 1);
                const float nqf = static_cast<float>(nq);
                ImFont* nfont = render::hud::PixelFontAt(nq);
                if (nfont == nullptr) nfont = font;
                const float nsize = pixel != nullptr ? 11.0f * nqf : 7.0f * q;
                const float ncapTop = pixel != nullptr ? 2.0f * nqf : 0.2f * nsize;
                const float capH = pixel != nullptr ? 7.0f * nqf : 0.55f * nsize;
                const ImVec2 ns = nfont->CalcTextSizeA(nsize, FLT_MAX, 0.0f, w.name.data());
                const float nw = pixel != nullptr ? ns.x - nqf : ns.x;
                float x0 = std::floor(tip.x + 0.5f * q - nw * 0.5f) - nqf;
                x0 = std::clamp(x0, c.x - inner, c.x + inner - nw - 2.0f * nqf);
                const float y1 = tip.y - 12.0f * q;
                draw->AddRectFilled(ImVec2(x0, y1 - capH - 2.0f * nqf), ImVec2(x0 + nw + 2.0f * nqf, y1), IM_COL32(0, 0, 0, 100));
                draw->AddText(nfont, nsize, ImVec2(x0 + nqf, y1 - capH - nqf - ncapTop), IM_COL32(255, 255, 255, 235), w.name.data());
            }
        }
    }

    // --- The key: save a waypoint where you are (only while playing: not
    // while typing in the chat or in a menu) ---
    {
        const bool down = addKey_ != keys::kNone && (GetAsyncKeyState(addKey_) & 0x8000) != 0;
        if (down && !keyWasDown_ && havePlayer && sdk::mouse::GameMouseState() == 1) {
            if (waypoints::HaveWorld()) {
                const std::size_t i = waypoints::Add(static_cast<int>(std::floor(p.feet[0])), static_cast<int>(std::floor(p.feet[1])),
                                                     static_cast<int>(std::floor(p.feet[2])));
                notify::Push(notify::Kind::Success, std::string(Tr("Waypoint saved: ")) + waypoints::List()[i].name.data());
            } else {
                notify::Push(notify::Kind::Warning, Tr("Waypoints: the world isn't known yet"));
            }
        }
        keyWasDown_ = down;
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
    if (sdk::blocks::CaveActive() && caveMode_ == 0) lineUnder(Tr("Cave"), rh::TextColor(0.7f));
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
    changed |= ui::Choice("Cave mode", &caveMode_, {"Auto", "Off", "Always"});
    ui::Hint("Auto: when there's a roof over you (a cave, a mine, the Nether), the map shows the floor at your level and "
             "the rock around it dark.");
    changed |= ui::Toggle("N, E, S, W", &cardinals_);
    changed |= ui::Toggle("Coordinates under it", &coords_);
    changed |= ui::Toggle("Show its cost", &showCost_, "(microseconds per frame)");
    ui::Hint("Move it and scale it with Edit HUD. It reads the blocks a little every frame on the game's own drawing "
             "thread, and only rereads what's new or old.");
    changed |= DrawWaypointSettings();
    return changed;
}

// The waypoints' part of the settings: the key, and the list of this world.
bool Minimap::DrawWaypointSettings() {
    bool changed = false;
    ui::Section("Waypoints");
    changed |= ui::Toggle("Show waypoints", &waypoints_);
    changed |= ui::Toggle("Names on the map", &waypointNames_);
    ui::Key("Save one where you are", &addKey_);
    if (!waypoints::HaveWorld()) {
        ui::Hint("Join a world: each world and each server has its own waypoints.");
        return changed;
    }
    char where[160];
    std::snprintf(where, sizeof(where), "%s: %s", Tr("World"), waypoints::WorldLabel().c_str());
    ui::Hint(where);
    sdk::player::Snapshot p;
    if (ui::Button("Add one here") && sdk::player::Read(p)) {
        waypoints::Add(static_cast<int>(std::floor(p.feet[0])), static_cast<int>(std::floor(p.feet[1])),
                       static_cast<int>(std::floor(p.feet[2])));
    }
    static const char* const kDims[] = {"Overworld", "Nether", "End"};
    auto& list = waypoints::List();
    std::size_t remove = list.size();
    for (std::size_t i = 0; i < list.size(); ++i) {
        waypoints::Waypoint& w = list[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Spacing();
        ImGui::Separator();
        bool edited = false;
        edited |= ui::Text("Name", w.name.data(), static_cast<int>(w.name.size()));
        edited |= ui::Color("Color", w.color);
        edited |= ui::Toggle("Visible", &w.visible);
        int xyz[3] = {w.x, w.y, w.z};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Tr("X, Y, Z"));
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", Tr(kDims[std::clamp(w.dimension, 0, 2)]));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt3("##xyz", xyz)) {
            w.x = xyz[0];
            w.y = std::clamp(xyz[1], -64, 320);
            w.z = xyz[2];
            edited = true;
        }
        if (ui::Button("Remove")) remove = i;
        if (edited) waypoints::MarkDirty();
        ImGui::PopID();
    }
    if (remove < list.size()) waypoints::Remove(remove);
    if (list.empty()) ui::Hint("No waypoints in this world yet.");
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
    out["cave"] = caveMode_;
    out["waypoints"] = waypoints_;
    out["waypointNames"] = waypointNames_;
    out["waypointKey"] = addKey_;
}

void Minimap::LoadSettings(const nlohmann::json& in) {
    size_ = std::clamp(in.value("size", 110.0f), 60.0f, 220.0f);
    showCost_ = in.value("cost", false);
    shape_ = std::clamp(in.value("shape", 1), 0, 1);
    rotate_ = in.value("rotate", false);
    zoom_ = std::clamp(in.value("zoom", 40.0f), 12.0f, 60.0f);
    cardinals_ = in.value("cardinals", true);
    coords_ = in.value("coords", false);
    caveMode_ = std::clamp(in.value("cave", 0), 0, 2);
    waypoints_ = in.value("waypoints", true);
    waypointNames_ = in.value("waypointNames", true);
    addKey_ = in.value("waypointKey", static_cast<int>('B'));
}

}  // namespace luma::modules::world
