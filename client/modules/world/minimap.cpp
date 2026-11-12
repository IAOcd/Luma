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
#include "core/config.h"
#include "core/logger.h"
#include "render/notifications.h"
#include "render/pixel_art.h"
#include "render/theme.h"
#include "render/overlay.h"
#include "render/icons.h"
#include "render/rainbow.h"
#include "render/ui.h"
#include "modules/world/map_look.h"
#include "modules/world/radar_style.h"
#include "modules/world/map_store.h"
#include "modules/world/waypoints.h"
#include "render/input.h"
#include "render/world_map.h"
#include "sdk/blocks.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"
#include "sdk/mouse_grab.h"
#include "sdk/paperdoll.h"
#include "sdk/network.h"
#include "sdk/player.h"
#include "sdk/radar.h"

namespace luma::modules::world {

namespace rh = render::hud;
namespace ui = render::ui;

namespace {

constexpr float kPi = 3.14159265358979f;

Minimap* g_minimap = nullptr;  // (for its page in the world map's sidebar)

ImU32 WithAlpha(ImU32 c, float a) {
    const ImU32 alpha = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

// The pixel-art pieces (banners, name tags) live in render/pixel_art.h.
using render::pixel::Banner;
using render::pixel::NameTag;
using render::pixel::PixelText;
using render::pixel::Shade;
using render::pixel::TextAt;

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
    g_minimap = this;
}

bool Minimap::MapPanel(render::pixel::Form& form) {
    return g_minimap != nullptr && g_minimap->DrawMapPanel(form);
}

const char* Minimap::IconSvg() const {
    return render::icons::kMapPin;
}

void Minimap::OnEnable() {
    sdk::blocks::Want(true);
    render::worldmap::SetMinimapPanel(&Minimap::MapPanel);
    version_ = 0;
}

void Minimap::OnDisable() {
    sdk::blocks::Want(false);
    sdk::radar::Want(false);
    render::worldmap::SetMinimapPanel(nullptr);
    waypoints::Flush();
    mapstore::Flush();
    sdk::draw3d::SetWorldShapes({}, sdk::draw3d::kChannelWaypoints);
    beamsSent_ = false;
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
    sdk::radar::Want(radar_ || render::worldmap::Config().entities);
    {
        const maplook::Look& look = maplook::Get();
        sdk::blocks::SetPainting(look.relief, look.heightTint, look.waterDepth, look.shores, look.texture);
        sdk::blocks::SetBiomeTint(look.biomes);
    }
    // A new world: forget the waypoints and the world map of the old one at
    // once (and wait until it's sure which world this is).
    if (sdk::blocks::WorldSession() != worldSession_) {
        worldSession_ = sdk::blocks::WorldSession();
        waypoints::ForgetWorld(now);
        mapstore::SetWorld("", -1);
    }
    // A new start (world or dimension): the picture of before is dropped.
    if (sdk::blocks::Session() != session_) {
        session_ = sdk::blocks::Session();
        std::fill(pixels_.begin(), pixels_.end(), 0u);
        version_ = 0;
        viewR_ = 0.0f;
    }
    waypoints::Update(now, sdk::blocks::Dimension());
    if (render::worldmap::TakeSettingsChanged()) config::MarkDirty();  // (the world map's settings live here)
    // The world map's memory follows the world and dimension at once (not
    // only when new blocks are read: standing still, nothing new comes).
    if (waypoints::HaveWorld() && sdk::blocks::Dimension() >= 0) mapstore::SetWorld(waypoints::StorageName(), sdk::blocks::Dimension());
    DrawWorldWaypoints();
    // --- Zoom (smooth) and where you are ---
    // How many blocks from the center to the edge. A turning square needs
    // its corners inside the 128 x 128 we know (so less).
    // (A turning square shows its corners too: they must fit in what's known.)
    const float maxRadius = (rotate_ && !round) ? 175.0f : 200.0f;
    const float target = std::clamp(zoom_, 12.0f, maxRadius);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    shownZoom_ = shownZoom_ <= 0.0f ? target : shownZoom_ + (target - shownZoom_) * std::min(1.0f, dt * 10.0f);
    sdk::player::Snapshot p;
    const bool havePlayer = sdk::player::Read(p);

    // --- The picture ---
    // 1. Every 0.2 s, the blocks around you from sdk/blocks (only as far as
    //    the zoom needs), also given to the world map's memory.
    // 2. THE MAP'S OWN TEXTURE (512 x 512): the square around you that the
    //    map shows, painted block by block (each block a crisp square, at
    //    any zoom). What the game hasn't loaded comes from the world map's
    //    memory (places you explored before). Painted again when new blocks
    //    arrive, when the zoom changes, or when you walked away from its center.
    const float needR = shownZoom_ * ((rotate_ && !round) ? 1.42f : 1.0f);
    {
        const int half = kGrid / 2;
        bool repaint = false;
        if (now >= nextUpload_) {
            nextUpload_ = now + 0.2;
            if (pixels_.size() != static_cast<std::size_t>(kGrid) * kGrid) pixels_.assign(static_cast<std::size_t>(kGrid) * kGrid, 0);
            int cx = 0, cz = 0;
            std::uint32_t v = 0;
            std::uint32_t copySession = 0;
            int copyDim = -1;
            if (sdk::blocks::CopyGrid(version_, pixels_.data(), cx, cz, v, areas_, &copySession, &copyDim)) {
                version_ = v;
                textureX_ = cx;
                textureZ_ = cz;
                // The world map remembers it, ONLY when it's sure where this is:
                // the world is known and the copy is of this world and
                // dimension. The surface; in the Nether, what cave mode sees
                // (there's always a roof there).
                const bool sure = waypoints::HaveWorld() && copySession == sdk::blocks::Session() && copyDim >= 0 &&
                                  copyDim == sdk::blocks::Dimension();
                if (sure && (!sdk::blocks::CaveActive() || copyDim == 1)) {
                    for (const sdk::blocks::Area& a : areas_) mapstore::MergeRing(pixels_.data(), kGrid, a.x0, a.z0, a.x1, a.z1);
                }
                repaint = true;
            }
        }
        const float wantR = std::min(static_cast<float>(half - 1), needR + 8.0f);
        const float pxNow = havePlayer ? p.feet[0] : static_cast<float>(textureX_);
        const float pzNow = havePlayer ? p.feet[2] : static_cast<float>(textureZ_);
        if (maplook::Version() != lookVersion_) {
            lookVersion_ = maplook::Version();
            repaint = true;
        }
        if (std::fabs(wantR - viewR_) > viewR_ * 0.08f || std::fabs(pxNow - static_cast<float>(viewX_)) > viewR_ * 0.12f ||
            std::fabs(pzNow - static_cast<float>(viewZ_)) > viewR_ * 0.12f || wantR > viewR_) {
            repaint = true;
        }
        if (repaint && !pixels_.empty()) {
            viewR_ = std::max(8.0f, std::ceil(wantR));
            viewX_ = static_cast<int>(std::floor(pxNow));
            viewZ_ = static_cast<int>(std::floor(pzNow));
            constexpr int kTex = 512;
            view_.resize(static_cast<std::size_t>(kTex) * kTex);
            const double step = 2.0 * viewR_ / kTex;  // blocks per texel
            const double x0 = viewX_ - viewR_, z0 = viewZ_ - viewR_;
            const bool cave = sdk::blocks::CaveActive();
            const bool plain = maplook::Plain();
            for (int j = 0; j < kTex; ++j) {
                const int bz = static_cast<int>(std::floor(z0 + (j + 0.5) * step));
                const bool rowIn = bz >= textureZ_ - half && bz < textureZ_ + half;
                const std::uint32_t* ringRow = pixels_.data() + static_cast<std::size_t>(bz & (kGrid - 1)) * kGrid;
                std::uint32_t* dst = view_.data() + static_cast<std::size_t>(j) * kTex;
                int lastBx = 0x7FFFFFFF;
                std::uint32_t color = 0;
                for (int i = 0; i < kTex; ++i) {
                    const int bx = static_cast<int>(std::floor(x0 + (i + 0.5) * step));
                    if (bx != lastBx) {
                        lastBx = bx;
                        // (the minimap's picture is a ring like sdk/blocks' grid: valid only inside the window)
                        color = (rowIn && bx >= textureX_ - half && bx < textureX_ + half) ? ringRow[bx & (kGrid - 1)] : 0;
                        if (color == 0 && !cave) color = mapstore::At(bx, bz, false);
                        if (color != 0 && !plain) color = maplook::Grade(color);
                    }
                    dst[i] = color;
                }
            }
            const auto* rgba = reinterpret_cast<const std::uint8_t*>(view_.data());
            if (texture_ == ImTextureID{}) {
                texture_ = render::gpu::CreateTexture(rgba, kTex, kTex);
            } else {
                render::gpu::UpdateTexture(texture_, rgba, kTex, kTex);
            }
        }
    }
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
        const float side = 2.0f * viewR_;
        return ImVec2((w.x - (static_cast<float>(viewX_) - viewR_)) / side, (w.y - (static_cast<float>(viewZ_) - viewR_)) / side);
    };
    // And the other way: a direction of the world -> the map (for N, E, S, W).
    auto toScreenDir = [&](float wx, float wz) {
        if (rotate_) return ImVec2(wx * right.x + wz * right.y, -(wx * forward.x + wz * forward.y));
        return ImVec2(wx, wz);
    };

    // --- Background and the map itself ---
    const rh::Style& style = rh::CurrentStyle();
    const ImU32 back = ImGui::ColorConvertFloat4ToU32(ImVec4(style.backgroundColor[0], style.backgroundColor[1], style.backgroundColor[2], backOpacity_));
    const float inner = radiusPx - 2.0f * u;  // the map, inside the frame
    if (round) {
        draw->AddCircleFilled(c, radiusPx, back, 64);
    } else {
        draw->AddRectFilled(o, ImVec2(o.x + side, o.y + side), back, 3.0f * u);
    }
    if (texture_ != ImTextureID{}) {
        // A fan: the center and the edge (a circle of 72 points, or the 4 corners).
        const int n = round ? 72 : 4;
        draw->PushTextureID(texture_);
        draw->PrimReserve(n * 3, n + 1);
        const ImDrawIdx base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
        const ImU32 white = IM_COL32(255, 255, 255, static_cast<int>(std::clamp(mapOpacity_, 0.0f, 1.0f) * 255.0f));
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
    const ImU32 ring = frameMode_ == 0   ? ImGui::ColorConvertFloat4ToU32(theme::Accent(1.0f))
                       : frameMode_ == 2 ? IM_COL32(static_cast<int>(frameColor_[0] * 255), static_cast<int>(frameColor_[1] * 255),
                                                    static_cast<int>(frameColor_[2] * 255), 255)
                                         : WithAlpha(rh::TextColor(), 0.55f);
    const float ringW = std::max(1.0f, u * static_cast<float>(frameWidth_));
    if (round) {
        if (frameMode_ != 3) draw->AddCircle(c, inner + 0.5f * ringW, ring, 72, ringW);
    } else {
        if (frameMode_ != 3) draw->AddRect(ImVec2(c.x - inner, c.y - inner), ImVec2(c.x + inner, c.y + inner), ring, 0.0f, 0, ringW);
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
                     north && northRed_ ? IM_COL32(240, 96, 80, 255) : rh::TextColor(0.85f));
        }
    }

    // --- The entities around you (LOCAL WORLDS ONLY: sdk/radar is empty on a server) ---
    if (radar_ && havePlayer) {
        sdk::radar::Copy(entities_);
        const float eq = std::max(1.0f, std::round(u * 0.75f));
        const float reach = inner - 3.0f * eq;
        for (const sdk::radar::Entity& e : entities_) {
            const sdk::radar::Kind kind = e.kind;
            using K = sdk::radar::Kind;
            if ((kind == K::Player && !radarPlayers_) || (kind == K::Hostile && !radarHostile_) ||
                ((kind == K::Animal || kind == K::Water || kind == K::Ambient) && !radarAnimals_) ||
                ((kind == K::Villager || kind == K::Other) && !radarOthers_) || (kind == K::Item && !radarItems_)) {
                continue;
            }
            const float dx = e.x - px, dz = e.z - pz;
            const ImVec2 d = rotate_ ? ImVec2((dx * right.x + dz * right.y) / blocksPerPx, -(dx * forward.x + dz * forward.y) / blocksPerPx)
                                     : ImVec2(dx / blocksPerPx, dz / blocksPerPx);
            const float k = round ? std::sqrt(d.x * d.x + d.y * d.y) : std::max(std::fabs(d.x), std::fabs(d.y));
            if (k > reach) continue;
            const radarstyle::Style st = radarstyle::Of(kind);
            const ImU32 color = radarHeight_ ? radarstyle::ByHeight(st.color, e.y - p.feet[1]) : st.color;
            render::pixel::Dot(draw, ImVec2(c.x + d.x, c.y + d.y), eq, color, st.shape, st.size + radarSize_);
        }
    }

    // --- Waypoints: a banner on the map, or a pixel-art arrow on its edge when far ---
    // The other way of toWorld: a point of the world -> pixels from the center.
    auto toScreen = [&](float wx, float wz) {
        const float dx = wx - px, dz = wz - pz;
        if (rotate_) return ImVec2((dx * right.x + dz * right.y) / blocksPerPx, -(dx * forward.x + dz * forward.y) / blocksPerPx);
        return ImVec2(dx / blocksPerPx, dz / blocksPerPx);
    };
    if (waypoints_ && havePlayer) {
        const int qi = std::clamp(static_cast<int>(std::lround(u)), 1, 8);
        const float q = static_cast<float>(qi);
        const PixelText text = TextAt(qi);
        const PixelText smallText = TextAt(std::max(1, qi - 1));  // names: smaller than the banner's letter
        const int dim = waypoints::Dimension();
        const float reach = inner - 6.0f * q;
        // FAR AWAY: a pixel-art arrow on the edge, pointing at the place,
        // painted cell by cell: each cell of a 13 x 13 grid whose center falls
        // inside the arrow's shape (turned toward the place) gets the color; a
        // dark outline around; the lower half a bit darker. So it stays made
        // of square pixels at any angle.
        auto inArrow = [](float x, float y, float grow) {
            const float tx = 3.6f * grow, bx = -3.2f * grow, by = 3.2f * grow, nx = -1.0f * grow;
            auto inTri = [](float x0, float y0, float ax, float ay, float bx0, float by0, float cx0, float cy0) {
                const float d1 = (x0 - bx0) * (ay - by0) - (ax - bx0) * (y0 - by0);
                const float d2 = (x0 - cx0) * (by0 - cy0) - (bx0 - cx0) * (y0 - cy0);
                const float d3 = (x0 - ax) * (cy0 - ay) - (cx0 - ax) * (y0 - ay);
                const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                return !(neg && pos);
            };
            return inTri(x, y, tx, 0.0f, bx, by, nx, 0.0f) || inTri(x, y, tx, 0.0f, nx, 0.0f, bx, -by);
        };
        auto arrow = [&](ImVec2 center, ImVec2 dir, const float* rgb) {
            const ImVec2 cell0(std::floor(center.x), std::floor(center.y));
            for (int gy = -6; gy <= 6; ++gy) {
                for (int gx = -6; gx <= 6; ++gx) {
                    const float x = static_cast<float>(gx), y = static_cast<float>(gy);
                    const float lx = x * dir.x + y * dir.y, ly = -x * dir.y + y * dir.x;
                    ImU32 color = 0;
                    if (inArrow(lx, ly, 1.0f)) {
                        color = ly > 0.0f ? Shade(rgb, 0.72f, 255) : Shade(rgb, 1.0f, 255);
                    } else if (inArrow(lx, ly, 1.5f)) {
                        color = IM_COL32(18, 18, 22, 230);
                    }
                    if (color == 0) continue;
                    const ImVec2 a0(cell0.x + x * q, cell0.y + y * q);
                    draw->AddRectFilled(a0, ImVec2(a0.x + q, a0.y + q), color);
                }
            }
        };
        int named = 0;
        for (const waypoints::Waypoint& w : waypoints::List()) {
            if (!w.visible || (dim >= 0 && w.dimension != dim)) continue;
            ImVec2 d = toScreen(static_cast<float>(w.x) + 0.5f, static_cast<float>(w.z) + 0.5f);
            const float k = round ? std::sqrt(d.x * d.x + d.y * d.y) : std::max(std::fabs(d.x), std::fabs(d.y));
            if (k > reach) {
                d.x *= reach / k;
                d.y *= reach / k;
                const float len = std::max(0.001f, std::sqrt(d.x * d.x + d.y * d.y));
                arrow(ImVec2(c.x + d.x, c.y + d.y), ImVec2(d.x / len, d.y / len), w.color);
                continue;
            }
            const ImVec2 tip(std::floor(c.x + d.x), std::floor(c.y + d.y));
            Banner(draw, tip, text, w.color, 255, w.name.data());
            // The name (at most 6, so the map stays readable).
            if (waypointNames_ && named < 6 && w.name[0] != 0) {
                ++named;
                NameTag(draw, tip.x + 0.5f * q, tip.y - 11.0f * q, smallText, w.name.data(), IM_COL32(255, 255, 255, 235), c.x - inner,
                        c.x + inner);
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
        // The world map (full screen): opens Luma's menu in "map mode".
        const bool mapDown = mapKey_ != keys::kNone && (GetAsyncKeyState(mapKey_) & 0x8000) != 0;
        if (mapDown && !mapKeyWasDown_ && sdk::mouse::GameMouseState() == 1 && !render::overlay::IsMenuOpen()) {
            render::worldmap::Open();
            render::input::RequestMenuToggle();
        }
        mapKeyWasDown_ = mapDown;
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
        draw->AddTriangleFilled(in(tip), in(left), in(notch), render::pixel::Shade(arrowColor_, 1.0f, 255));
        draw->AddTriangleFilled(in(tip), in(notch), in(rightPt), render::pixel::Shade(arrowColor_, 0.86f, 255));
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
    changed |= ui::Slider("Zoom (blocks to the edge)", &zoom_, 12.0f, 200.0f, "%.0f");
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

// --- Its page in the world map's sidebar: everything to customize it ---
bool Minimap::DrawMapPanel(render::pixel::Form& f) {
    bool changed = false;
    static const char* const kShapes[] = {"Square", "Round"};
    const char* shapes[] = {Tr(kShapes[0]), Tr(kShapes[1])};
    changed |= f.Choice(Tr("Shape"), &shape_, shapes, 2);
    changed |= f.Slider(Tr("Size"), &size_, 60.0f, 220.0f, "%.0f");
    changed |= f.Slider(Tr("Zoom (blocks to the edge)"), &zoom_, 12.0f, 200.0f, "%.0f");
    changed |= f.Toggle(Tr("Rotate with you"), &rotate_);
    f.Section(Tr("Frame"));
    const char* frames[] = {Tr("Accent"), Tr("Text"), Tr("Color"), Tr("None")};
    changed |= f.Choice(Tr("Ring"), &frameMode_, frames, 4);
    if (frameMode_ == 2) changed |= f.Colors(Tr("Ring color"), frameColor_);
    if (frameMode_ != 3) changed |= f.SliderInt(Tr("Ring width"), &frameWidth_, 1, 4, "%d px");
    changed |= f.Slider(Tr("Background"), &backOpacity_, 0.0f, 1.0f, "%.2f");
    changed |= f.Slider(Tr("Map opacity"), &mapOpacity_, 0.3f, 1.0f, "%.2f");
    f.Section(Tr("On the map"));
    changed |= f.Toggle(Tr("N, E, S, W"), &cardinals_);
    changed |= f.Toggle(Tr("Red N"), &northRed_);
    changed |= f.Toggle(Tr("Coordinates under it"), &coords_);
    changed |= f.Colors(Tr("Your arrow"), arrowColor_);
    changed |= f.Toggle(Tr("Show its cost"), &showCost_);
    f.Section(Tr("Cave mode"));
    const char* caves[] = {Tr("Auto"), Tr("Off"), Tr("Always")};
    changed |= f.Choice(Tr("When"), &caveMode_, caves, 3);
    f.Section(Tr("Entity radar"));
    if (sdk::network::OnServer()) f.Hint(Tr("Only in your own worlds: on a server the radar shows nothing."));
    changed |= f.Toggle(Tr("Show entities"), &radar_);
    changed |= f.Toggle(Tr("Players"), &radarPlayers_);
    changed |= f.Toggle(Tr("Monsters"), &radarHostile_);
    changed |= f.Toggle(Tr("Animals"), &radarAnimals_);
    changed |= f.Toggle(Tr("Villagers and others"), &radarOthers_);
    changed |= f.Toggle(Tr("Items on the ground"), &radarItems_);
    changed |= f.SliderInt(Tr("Icon size"), &radarSize_, 0, 2, "+%d");
    changed |= f.Toggle(Tr("Lighter above, darker below"), &radarHeight_);
    f.Section(Tr("Waypoints"));
    changed |= f.Toggle(Tr("Show waypoints"), &waypoints_);
    changed |= f.Toggle(Tr("Names on the map"), &waypointNames_);
    changed |= f.Toggle(Tr("Beams in the world"), &beams_);
    changed |= f.Toggle(Tr("Labels in the world"), &worldLabels_);
    changed |= f.Toggle(Tr("Death point"), &deathPoint_);
    f.Hint(Tr("The keys (save a waypoint, open this map) are in Luma's menu, in the Minimap module."));
    return changed;
}

// The waypoints' part of the settings: the key, and the list of this world.
// --- Where you died (every 50 ms, from Luma's main thread, with the state lock) ---
// Here and not where the map is drawn: while you're dead the game hides its
// HUD, and so does Luma, so the drawing code doesn't run at that moment.
// Two signs, whichever comes first:
//  1. A screen with "death" in its name is drawn (if the game draws it
//     through the function Luma watches; in 1.26.63 it doesn't seem to).
//  2. THE GAME LET GO OF THE MOUSE, but none of the screens Luma knows
//     (pause, chat, inventory...) is being drawn, nor Luma's menu: some
//     other screen is up. If, when the mouse comes back, you are somewhere
//     else (more than 3 blocks away, same dimension), you respawned: you died.
// The point saved is the last place you stood while playing.
void Minimap::OnTick() {
    mapstore::Tick(rainbow::Seconds());  // saves one changed region of the world map, if any
    sdk::player::Snapshot p;
    if (!sdk::player::Read(p)) return;
    const double now = rainbow::Seconds();
    auto saveDeath = [&]() {
        if (now - lastDeath_ < 5.0) return;
        lastDeath_ = now;
        if (deathPoint_ && waypoints::HaveWorld()) {
            waypoints::SetDeathPoint(static_cast<int>(std::floor(alive_[0])), static_cast<int>(std::floor(alive_[1])),
                                     static_cast<int>(std::floor(alive_[2])));
            notify::Push(notify::Kind::Info, Tr("Death point saved"));
            log::Info("Waypoints: death point at {:.0f}, {:.0f}, {:.0f}.", alive_[0], alive_[1], alive_[2]);
        }
    };
    const unsigned deaths = sdk::paperdoll::DeathScreenFrames();
    if (deathFrames_ == 0xFFFFFFFFu) deathFrames_ = deaths;
    if (deaths != deathFrames_) {
        deathFrames_ = deaths;
        saveDeath();
    }
    const int mouse = sdk::mouse::GameMouseState();
    const unsigned menus = sdk::paperdoll::MenuScreenFrames();
    const int dim = sdk::blocks::Dimension();
    if (mouse == 0 && !render::overlay::IsMenuOpen()) {
        if (releasedAt_ < 0.0 || menus != menusSeen_) {
            releasedAt_ = now;  // (a known screen is open: start counting again)
            menusSeen_ = menus;
        } else if (now - releasedAt_ > 0.4 && !hiddenScreen_) {
            hiddenScreen_ = true;
            hiddenWorld_ = sdk::blocks::WorldSession();
            log::Info("Waypoints: the mouse is free and no known screen is drawn (death screen?).");
        }
    } else if (mouse == 1) {
        if (hiddenScreen_) {
            const float ddx = p.feet[0] - alive_[0], ddy = p.feet[1] - alive_[1], ddz = p.feet[2] - alive_[2];
            const float moved = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
            log::Info("Waypoints: back to playing, {:.1f} blocks from before.", moved);
            // (Not if you went to another world meanwhile: that's a loading screen.)
            if (moved > 3.0f && dim == aliveDim_ && hiddenWorld_ == sdk::blocks::WorldSession()) saveDeath();
        }
        hiddenScreen_ = false;
        releasedAt_ = -1.0;
        alive_[0] = p.feet[0];
        alive_[1] = p.feet[1];
        alive_[2] = p.feet[2];
        aliveDim_ = dim;
    }
}

// --- Waypoints IN THE WORLD -------------------------------------------------------------
// A BEAM: a thin column of light of the waypoint's color, from the place up
// to the sky. It's drawn by the game with the rest of the world (sdk/draw3d,
// the same lines as the block outline), so hills and walls in front cover it.
// Its lines are always 3 screen pixels apart, so it looks the same thin line
// near or far.
// A LABEL above it: the banner, the name and how far it is, drawn on top of
// the picture (seen through walls, like a compass). Far ones fade a little.
void Minimap::DrawWorldWaypoints() {
    sdk::camera::View view;
    const bool any = waypoints_ && (beams_ || worldLabels_) && sdk::camera::GetView(view);
    if (!any) {
        if (beamsSent_) sdk::draw3d::SetWorldShapes({}, sdk::draw3d::kChannelWaypoints);
        beamsSent_ = false;
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float yaw = view.yaw * kPi / 180.0f, pitch = view.pitch * kPi / 180.0f;
    const float fwd[3] = {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const float rgt[3] = {-std::cos(yaw), 0.0f, -std::sin(yaw)};
    // up = right x forward
    const float up[3] = {rgt[1] * fwd[2] - rgt[2] * fwd[1], rgt[2] * fwd[0] - rgt[0] * fwd[2], rgt[0] * fwd[1] - rgt[1] * fwd[0]};
    auto project = [&](const float p[3], ImVec2& out, float& depth) {
        const float d[3] = {p[0] - view.pos[0], p[1] - view.pos[1], p[2] - view.pos[2]};
        depth = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (depth < 0.2f) return false;
        const float x = (d[0] * rgt[0] + d[1] * rgt[1] + d[2] * rgt[2]) * view.scaleX / depth;
        const float y = (d[0] * up[0] + d[1] * up[1] + d[2] * up[2]) * view.scaleY / depth;
        out = ImVec2((1.0f + x) * 0.5f * display.x, (1.0f - y) * 0.5f * display.y);
        return true;
    };
    const rh::Metrics m = rh::Measure(1.0f);
    const int qi = std::clamp(static_cast<int>(std::lround(m.px)), 1, 8);
    const float q = static_cast<float>(qi);
    const PixelText text = TextAt(qi);
    const PixelText smallText = TextAt(std::max(1, qi - 1));
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const int dim = waypoints::Dimension();
    std::vector<sdk::draw3d::WorldShapes> shapes;
    // The labels on screen this frame (kept between frames: no new memory each frame).
    struct Label {
        const waypoints::Waypoint* w;
        ImVec2 tip;
        float dist;
    };
    static std::vector<Label> labels;
    labels.clear();
    for (const waypoints::Waypoint& w : waypoints::List()) {
        if (!w.visible || !w.inWorld || (dim >= 0 && w.dimension != dim)) continue;
        const float wp[3] = {static_cast<float>(w.x) + 0.5f, static_cast<float>(w.y), static_cast<float>(w.z) + 0.5f};
        const float dx = wp[0] - view.pos[0], dy = wp[1] - view.pos[1], dz = wp[2] - view.pos[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (beams_ && dist > 2.0f) {
            sdk::draw3d::WorldShapes beam;
            beam.lines = true;
            beam.color = {w.color[0], w.color[1], w.color[2], 0.85f};
            const float depth = std::max(1.0f, dx * fwd[0] + dy * fwd[1] + dz * fwd[2]);
            const float pixel = depth / (view.scaleX * display.x * 0.5f);  // world size of one screen pixel there
            const float top = std::max(wp[1] + 2.0f, 320.0f);
            for (int i = -1; i <= 1; ++i) {
                const float ox = rgt[0] * pixel * static_cast<float>(i), oz = rgt[2] * pixel * static_cast<float>(i);
                beam.triangles.insert(beam.triangles.end(), {wp[0] + ox, wp[1], wp[2] + oz, wp[0] + ox, top, wp[2] + oz});
            }
            shapes.push_back(std::move(beam));
        }
        if (!worldLabels_) continue;
        // The label: two blocks above the place (or at your eyes' height if
        // the place is far below or above, so it stays on screen).
        float lp[3] = {wp[0], wp[1] + 2.2f, wp[2]};
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        if (horizontal > 24.0f) lp[1] = std::clamp(lp[1], view.pos[1] - horizontal * 0.25f, view.pos[1] + horizontal * 0.25f);
        ImVec2 at;
        float depth = 0.0f;
        if (!project(lp, at, depth)) continue;
        if (at.x < -40.0f || at.x > display.x + 40.0f || at.y < -40.0f || at.y > display.y + 40.0f) continue;
        labels.push_back(Label{&w, ImVec2(std::floor(at.x), std::floor(at.y)), dist});
    }
    // THE ONE YOU LOOK AT (the label nearest the middle of the screen, if
    // it's close to the crosshair) grows one pixel size and is drawn last,
    // on top of the others.
    int looked = -1;
    float best = display.y * 0.06f;
    best *= best;
    const ImVec2 mid(display.x * 0.5f, display.y * 0.5f);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        // (the label's middle: a little above its tip)
        const float ddx = labels[i].tip.x - mid.x, ddy = labels[i].tip.y - 5.0f * q - mid.y;
        const float d2 = ddx * ddx + ddy * ddy;
        if (d2 < best) {
            best = d2;
            looked = static_cast<int>(i);
        }
    }
    const PixelText bigText = TextAt(std::min(8, qi + 1));
    auto drawLabel = [&](const Label& l, bool big) {
        const PixelText& t = big ? bigText : text;
        const PixelText& st = big ? text : smallText;
        const float lq = t.q;
        const int alpha = big ? 255 : (l.dist > 256.0f ? 170 : 235);
        Banner(draw, l.tip, t, l.w->color, alpha, l.w->name.data());
        NameTag(draw, l.tip.x + 0.5f * lq, l.tip.y - 11.0f * lq, st, l.w->name.data(), IM_COL32(255, 255, 255, alpha), -1.0e6f, 1.0e6f);
        char meters[32];
        std::snprintf(meters, sizeof(meters), "%.0f m", l.dist);
        NameTag(draw, l.tip.x + 0.5f * lq, l.tip.y + 2.0f * lq + st.capHeight + 2.0f * st.q, st, meters, IM_COL32(220, 220, 220, alpha),
                -1.0e6f, 1.0e6f);
    };
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (static_cast<int>(i) != looked) drawLabel(labels[i], false);
    }
    if (looked >= 0) drawLabel(labels[static_cast<std::size_t>(looked)], true);
    sdk::draw3d::SetWorldShapes(std::move(shapes), sdk::draw3d::kChannelWaypoints);
    beamsSent_ = true;
}

bool Minimap::DrawWaypointSettings() {
    bool changed = false;
    ui::Section("Waypoints");
    changed |= ui::Toggle("Show waypoints", &waypoints_);
    changed |= ui::Toggle("Names on the map", &waypointNames_);
    changed |= ui::Toggle("Beams in the world", &beams_, "(a column of light; hills and walls cover it)");
    changed |= ui::Toggle("Labels in the world", &worldLabels_, "(name and distance, seen through walls)");
    changed |= ui::Toggle("Death point", &deathPoint_, "(a waypoint where you died; only the last one)");
    ui::Key("Save one where you are", &addKey_);
    ui::Key("World map", &mapKey_, "(full screen: everything you explored)");
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
        edited |= ui::Toggle("On the minimap", &w.visible);
        edited |= ui::Toggle("In the world", &w.inWorld, "(its beam and label)");
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
    out["mapKey"] = mapKey_;
    const render::worldmap::Settings& wm = render::worldmap::Config();
    out["worldmapEntities"] = wm.entities;
    out["worldmap"] = {{"waypoints", wm.waypoints}, {"names", wm.names},       {"bannerSize", wm.bannerSize}, {"deathPoint", wm.deathPoint},
                       {"chunkGrid", wm.chunkGrid}, {"regionGrid", wm.regionGrid}, {"rulers", wm.rulers},        {"coords", wm.coords},
                       {"follow", wm.follow},       {"cost", wm.cost},             {"zoomSpeed", wm.zoomSpeed},  {"startZoom", wm.startZoom},
                       {"remember", wm.remember},   {"sidebar", wm.sidebar},       {"tab", wm.tab}};
    maplook::Save(out["look"]);
    out["frameMode"] = frameMode_;
    out["frameColor"] = {frameColor_[0], frameColor_[1], frameColor_[2]};
    out["frameWidth"] = frameWidth_;
    out["backOpacity"] = backOpacity_;
    out["mapOpacity"] = mapOpacity_;
    out["arrowColor"] = {arrowColor_[0], arrowColor_[1], arrowColor_[2]};
    out["northRed"] = northRed_;
    out["radar"] = {{"on", radar_},         {"players", radarPlayers_}, {"hostile", radarHostile_}, {"animals", radarAnimals_},
                    {"others", radarOthers_}, {"items", radarItems_},     {"size", radarSize_},       {"height", radarHeight_}};
    out["beams"] = beams_;
    out["worldLabels"] = worldLabels_;
    out["deathPoint"] = deathPoint_;
}

void Minimap::LoadSettings(const nlohmann::json& in) {
    size_ = std::clamp(in.value("size", 110.0f), 60.0f, 220.0f);
    showCost_ = in.value("cost", false);
    shape_ = std::clamp(in.value("shape", 1), 0, 1);
    rotate_ = in.value("rotate", false);
    zoom_ = std::clamp(in.value("zoom", 40.0f), 12.0f, 200.0f);
    cardinals_ = in.value("cardinals", true);
    coords_ = in.value("coords", false);
    caveMode_ = std::clamp(in.value("cave", 0), 0, 2);
    waypoints_ = in.value("waypoints", true);
    waypointNames_ = in.value("waypointNames", true);
    addKey_ = in.value("waypointKey", static_cast<int>('B'));
    mapKey_ = in.value("mapKey", static_cast<int>('M'));
    if (in.contains("worldmap") && in["worldmap"].is_object()) {
        const auto& j = in["worldmap"];
        render::worldmap::Settings& wm = render::worldmap::Config();
        wm.waypoints = j.value("waypoints", true);
        wm.names = j.value("names", true);
        wm.chunkGrid = j.value("chunkGrid", false);
        wm.coords = j.value("coords", true);
        wm.follow = j.value("follow", false);
        wm.cost = j.value("cost", false);
        wm.bannerSize = std::clamp(j.value("bannerSize", 0), 0, 2);
        wm.deathPoint = j.value("deathPoint", true);
        wm.regionGrid = j.value("regionGrid", false);
        wm.rulers = j.value("rulers", true);
        wm.zoomSpeed = std::clamp(j.value("zoomSpeed", 1.0f), 0.25f, 2.0f);
        wm.startZoom = std::clamp(j.value("startZoom", 2.0f), 0.25f, 8.0f);
        wm.remember = j.value("remember", true);
        wm.sidebar = j.value("sidebar", true);
        wm.tab = std::clamp(j.value("tab", 0), 0, 4);
    }
    if (in.contains("look")) maplook::Load(in["look"]);
    render::worldmap::Config().entities = in.value("worldmapEntities", true);
    auto color3 = [&](const char* key, float* out3) {
        if (in.contains(key) && in[key].is_array() && in[key].size() == 3) {
            for (int k = 0; k < 3; ++k) out3[k] = std::clamp(in[key][k].get<float>(), 0.0f, 1.0f);
        }
    };
    frameMode_ = std::clamp(in.value("frameMode", 1), 0, 3);
    color3("frameColor", frameColor_);
    frameWidth_ = std::clamp(in.value("frameWidth", 1), 1, 4);
    backOpacity_ = std::clamp(in.value("backOpacity", 0.6f), 0.0f, 1.0f);
    mapOpacity_ = std::clamp(in.value("mapOpacity", 1.0f), 0.3f, 1.0f);
    color3("arrowColor", arrowColor_);
    northRed_ = in.value("northRed", true);
    if (in.contains("radar") && in["radar"].is_object()) {
        const auto& r = in["radar"];
        radar_ = r.value("on", true);
        radarPlayers_ = r.value("players", true);
        radarHostile_ = r.value("hostile", true);
        radarAnimals_ = r.value("animals", true);
        radarOthers_ = r.value("others", true);
        radarItems_ = r.value("items", false);
        radarSize_ = std::clamp(r.value("size", 0), 0, 2);
        radarHeight_ = r.value("height", true);
    }
    beams_ = in.value("beams", true);
    worldLabels_ = in.value("worldLabels", true);
    deathPoint_ = in.value("deathPoint", true);
}

}  // namespace luma::modules::world
