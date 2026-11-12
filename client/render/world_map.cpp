// =============================================================================
//  world_map.cpp - The world map, full screen (see world_map.h)
// =============================================================================
#include "render/world_map.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

#include "core/logger.h"
#include "core/tr.h"
#include "modules/world/map_store.h"
#include "modules/world/waypoints.h"
#include "perf/timer.h"
#include "render/gpu.h"
#include "render/hud.h"
#include "render/input.h"
#include "sdk/player.h"

namespace luma::render::worldmap {

namespace {

namespace mapstore = modules::world::mapstore;
namespace waypoints = modules::world::waypoints;

constexpr float kPi = 3.14159265358979f;
constexpr float kMinZoom = 0.125f;  // screen pixels per block (8 blocks per pixel)
constexpr float kMaxZoom = 16.0f;   // (16 pixels per block)
constexpr std::uint32_t kEmpty = 0xFF161412u;  // unexplored: almost black (RGBA, R first)

bool g_active = false;
double g_centerX = 0.0, g_centerZ = 0.0;  // the world point in the middle of the screen
float g_zoom = 2.0f, g_shownZoom = 2.0f;  // pixels per block (shown: animated toward it)
ImTextureID g_texture{};
int g_texW = 0, g_texH = 0;
int g_textures = 0;  // how many were made (a new one only if the screen size changes)
std::vector<std::uint32_t> g_pixels;
// What the texture shows now (to fill it again only when something changed).
double g_drawnX = 1e300, g_drawnZ = 1e300;
float g_drawnZoom = -1.0f;
std::uint32_t g_drawnVersion = 0;
float g_fillMs = 0.0f;
double g_lastFill = 0.0;

// Fills the texture with the view: for each pixel, the block under it.
// FAST ON PURPOSE (2 million pixels on a 1080p screen):
//  - which block column each pixel column shows is worked out once per
//    picture (not once per row);
//  - the colors are read a whole region row at a time (no lookup per pixel);
//  - when zoomed in, many screen rows show the same row of blocks: that row
//    is copied from the one above instead of being worked out again.
std::vector<int> g_colBlock;   // pixel column -> block x
std::vector<int> g_colRegion;  // pixel column -> region x
void Fill(int w, int h) {
    g_pixels.resize(static_cast<std::size_t>(w) * h);
    g_colBlock.resize(static_cast<std::size_t>(w));
    g_colRegion.resize(static_cast<std::size_t>(w));
    const double inv = 1.0 / g_drawnZoom;
    const double left = g_drawnX - w * 0.5 * inv, top = g_drawnZ - h * 0.5 * inv;
    for (int x = 0; x < w; ++x) {
        const int bx = static_cast<int>(std::floor(left + (x + 0.5) * inv));
        g_colBlock[static_cast<std::size_t>(x)] = bx;
        g_colRegion[static_cast<std::size_t>(x)] = mapstore::RegionOf(bx);
    }
    int lastBz = 0x7FFFFFFF;
    for (int y = 0; y < h; ++y) {
        std::uint32_t* row = g_pixels.data() + static_cast<std::size_t>(y) * w;
        const int bz = static_cast<int>(std::floor(top + (y + 0.5) * inv));
        if (bz == lastBz) {
            std::memcpy(row, row - w, sizeof(std::uint32_t) * static_cast<std::size_t>(w));
            continue;
        }
        lastBz = bz;
        const int rz = mapstore::RegionOf(bz);
        int curRx = 0x7FFFFFFF;
        const std::uint32_t* src = nullptr;
        for (int x = 0; x < w; ++x) {
            const int rx = g_colRegion[static_cast<std::size_t>(x)];
            if (rx != curRx) {
                curRx = rx;
                src = mapstore::Row(rx, rz, bz);
            }
            const std::uint32_t v = src != nullptr ? src[g_colBlock[static_cast<std::size_t>(x)] - rx * mapstore::kRegion] : 0;
            row[x] = v != 0 ? v : kEmpty;
        }
    }
}

// The pixel-art arrow of the player, turned toward "yaw" (the game's: 0 = south).
void DrawPlayer(ImDrawList* draw, ImVec2 at, float yaw, float q) {
    const float a = yaw * kPi / 180.0f;
    const ImVec2 dir(-std::sin(a), std::cos(a));
    auto inTri = [](float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
        const float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
        const float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
        const float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
        return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
    };
    auto inArrow = [&](float x, float y, float g) {
        return inTri(x, y, 4.0f * g, 0, -3.5f * g, 3.5f * g, -1.2f * g, 0) || inTri(x, y, 4.0f * g, 0, -1.2f * g, 0, -3.5f * g, -3.5f * g);
    };
    const ImVec2 c0(std::floor(at.x), std::floor(at.y));
    for (int gy = -7; gy <= 7; ++gy) {
        for (int gx = -7; gx <= 7; ++gx) {
            const float x = static_cast<float>(gx), y = static_cast<float>(gy);
            const float lx = x * dir.x + y * dir.y, ly = -x * dir.y + y * dir.x;
            ImU32 col = 0;
            if (inArrow(lx, ly, 1.0f)) {
                col = ly > 0.0f ? IM_COL32(205, 205, 210, 255) : IM_COL32(255, 255, 255, 255);
            } else if (inArrow(lx, ly, 1.45f)) {
                col = IM_COL32(16, 16, 20, 235);
            }
            if (col == 0) continue;
            const ImVec2 p(c0.x + x * q, c0.y + y * q);
            draw->AddRectFilled(p, ImVec2(p.x + q, p.y + q), col);
        }
    }
}

// Text in the game's font with its shadow, at (x, y) = the top of the capitals.
void PixelText(ImDrawList* draw, ImFont* font, float q, ImVec2 pos, const char* text, ImU32 color) {
    const float size = 11.0f * q, capTop = 2.0f * q;
    const ImVec2 p(std::floor(pos.x), std::floor(pos.y - capTop));
    draw->AddText(font, size, ImVec2(p.x + q, p.y + q), IM_COL32(0, 0, 0, 150), text);
    draw->AddText(font, size, p, color, text);
}

float PixelWidth(ImFont* font, float q, const char* text) {
    return font->CalcTextSizeA(11.0f * q, FLT_MAX, 0.0f, text).x - q;
}

}  // namespace

bool IsActive() {
    return g_active;
}

void Open() {
    sdk::player::Snapshot p;
    if (sdk::player::Read(p)) {
        g_centerX = p.feet[0];
        g_centerZ = p.feet[2];
    }
    g_shownZoom = g_zoom;
    g_drawnZoom = -1.0f;  // (fill it again)
    g_active = true;
}

void Close() {
    g_active = false;
}

void Draw() {
    if (!g_active) return;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const int w = std::max(1, static_cast<int>(display.x)), h = std::max(1, static_cast<int>(display.y));
    const float q = std::max(1.0f, std::round(display.y / 540.0f));  // one pixel of the game's interface
    ImFont* font = hud::PixelFontAt(static_cast<int>(q));
    if (font == nullptr) font = hud::SmoothFont();

    // A window over the whole screen: it takes the mouse (the menu's own
    // window isn't drawn while the map is open).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##luma_worldmap", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##map", display, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();

    // --- Moving and zooming ---
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        g_centerX -= io.MouseDelta.x / g_shownZoom;
        g_centerZ -= io.MouseDelta.y / g_shownZoom;
    }
    const ImVec2 mouse = io.MousePos;
    auto worldAt = [&](ImVec2 s, float zoom, double& wx, double& wz) {
        wx = g_centerX + (s.x - display.x * 0.5f) / zoom;
        wz = g_centerZ + (s.y - display.y * 0.5f) / zoom;
    };
    if (hovered && io.MouseWheel != 0.0f) {
        g_zoom = std::clamp(g_zoom * std::pow(2.0f, io.MouseWheel * 0.5f), kMinZoom, kMaxZoom);
    }
    if (std::fabs(g_shownZoom - g_zoom) > 0.0001f) {
        // Smooth, and the point under the cursor stays under the cursor.
        double bx, bz;
        worldAt(mouse, g_shownZoom, bx, bz);
        const float k = std::min(1.0f, io.DeltaTime * 14.0f);
        g_shownZoom = std::fabs(g_zoom - g_shownZoom) < g_zoom * 0.01f ? g_zoom : g_shownZoom + (g_zoom - g_shownZoom) * k;
        if (hovered) {
            g_centerX = bx - (mouse.x - display.x * 0.5f) / g_shownZoom;
            g_centerZ = bz - (mouse.y - display.y * 0.5f) / g_shownZoom;
        }
    }
    sdk::player::Snapshot p;
    const bool havePlayer = sdk::player::Read(p);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && havePlayer) {
        g_centerX = p.feet[0];
        g_centerZ = p.feet[2];
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        input::RequestMenuToggle();  // (closing the menu closes the map: menu::OnClosed)
    }

    // --- The picture: filled again only when the view or the blocks changed ---
    {
        const double halfW = w * 0.5 / g_shownZoom, halfH = h * 0.5 / g_shownZoom;
        mapstore::Prefetch(static_cast<int>(std::floor(g_centerX - halfW)), static_cast<int>(std::floor(g_centerZ - halfH)),
                           static_cast<int>(std::floor(g_centerX + halfW)), static_cast<int>(std::floor(g_centerZ + halfH)), 2);
    }
    // WHILE YOU DRAG OR ZOOM, the picture already made is just moved and
    // scaled by the GPU (almost free); the crisp one is made again at most 8
    // times a second, and right away when you stop.
    // THE PICTURE IS BIGGER THAN THE SCREEN (a margin of a quarter of the
    // screen's height on every side). While you drag or zoom, the GPU just
    // moves and scales it: smooth. It's made again only when you stop, or
    // when the screen would show past its edge.
    const int margin = h / 4;
    const int tw = w + 2 * margin, th = h + 2 * margin;
    const bool moved = g_centerX != g_drawnX || g_centerZ != g_drawnZ || g_shownZoom != g_drawnZoom;
    const bool fresh = mapstore::Version() != g_drawnVersion;
    const bool interacting = ImGui::IsItemActive() || std::fabs(g_shownZoom - g_zoom) > 0.0001f;
    const double now = ImGui::GetTime();
    // Does the picture still cover the whole screen?
    bool covers = false;
    if (g_texture != ImTextureID{} && g_drawnZoom > 0.0f) {
        const double halfW = tw * 0.5 / g_drawnZoom, halfH = th * 0.5 / g_drawnZoom;
        const double viewW = w * 0.5 / g_shownZoom, viewH = h * 0.5 / g_shownZoom;
        covers = g_centerX - viewW >= g_drawnX - halfW && g_centerX + viewW <= g_drawnX + halfW && g_centerZ - viewH >= g_drawnZ - halfH &&
                 g_centerZ + viewH <= g_drawnZ + halfH && g_shownZoom <= g_drawnZoom * 1.6f;
    }
    const bool due = interacting ? (!covers && now - g_lastFill > 0.25) : (moved || fresh);
    if (due || tw != g_texW || th != g_texH || g_texture == ImTextureID{}) {
        const std::int64_t t0 = perf::Now();
        g_lastFill = now;
        g_drawnX = g_centerX;
        g_drawnZ = g_centerZ;
        g_drawnZoom = g_shownZoom;
        Fill(tw, th);
        const auto* rgba = reinterpret_cast<const std::uint8_t*>(g_pixels.data());
        if (g_texture == ImTextureID{} || tw != g_texW || th != g_texH) {
            if (g_textures < 4) {
                g_texture = gpu::CreateTexture(rgba, static_cast<UINT>(tw), static_cast<UINT>(th));
                ++g_textures;
                g_texW = tw;
                g_texH = th;
            }
        } else {
            gpu::UpdateTexture(g_texture, rgba, static_cast<UINT>(tw), static_cast<UINT>(th));
        }
        g_fillMs = static_cast<float>(perf::TicksToMs(perf::Now() - t0));
        g_drawnVersion = mapstore::Version();
    }
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(18, 20, 22, 255));
    if (g_texture != ImTextureID{} && g_texW == tw && g_texH == th) {
        // Where the picture's view sits in the current view (to the fraction
        // of a pixel while moving: smooth; on whole pixels when still: crisp).
        const float k = g_shownZoom / g_drawnZoom;
        const float x0 = static_cast<float>((g_drawnX - tw * 0.5 / g_drawnZoom - g_centerX) * g_shownZoom + display.x * 0.5);
        const float y0 = static_cast<float>((g_drawnZ - th * 0.5 / g_drawnZoom - g_centerZ) * g_shownZoom + display.y * 0.5);
        const ImVec2 a = interacting ? ImVec2(x0, y0) : ImVec2(std::round(x0), std::round(y0));
        draw->AddImage(g_texture, a, ImVec2(a.x + static_cast<float>(tw) * k, a.y + static_cast<float>(th) * k));
    }

    // --- You ---
    auto toScreen = [&](double wx, double wz) {
        return ImVec2(static_cast<float>((wx - g_centerX) * g_shownZoom + display.x * 0.5f),
                      static_cast<float>((wz - g_centerZ) * g_shownZoom + display.y * 0.5f));
    };
    if (havePlayer) {
        ImVec2 at = toScreen(p.feet[0], p.feet[2]);
        // Off the screen: on its edge, so you always know where you are.
        at.x = std::clamp(at.x, 10.0f * q, display.x - 10.0f * q);
        at.y = std::clamp(at.y, 10.0f * q, display.y - 10.0f * q);
        DrawPlayer(draw, at, p.yaw, q);
    }

    // --- Texts: the world on top, the block under the cursor and the keys at the bottom ---
    {
        static const char* const kDims[] = {"Overworld", "Nether", "End"};
        const int dim = waypoints::Dimension();
        char title[200];
        std::snprintf(title, sizeof(title), "%s  -  %s", waypoints::HaveWorld() ? waypoints::WorldLabel().c_str() : Tr("World map"),
                      Tr(kDims[std::clamp(dim, 0, 2)]));
        const float titleW = PixelWidth(font, q, title);
        draw->AddRectFilled(ImVec2(display.x * 0.5f - titleW * 0.5f - 4 * q, 4 * q), ImVec2(display.x * 0.5f + titleW * 0.5f + 4 * q, 15 * q),
                            IM_COL32(0, 0, 0, 120));
        PixelText(draw, font, q, ImVec2(display.x * 0.5f - titleW * 0.5f, 6 * q), title, IM_COL32(255, 255, 255, 255));

        double mx, mz;
        worldAt(mouse, g_shownZoom, mx, mz);
        char under[96];
        std::snprintf(under, sizeof(under), "X: %d   Z: %d", static_cast<int>(std::floor(mx)), static_cast<int>(std::floor(mz)));
        const float uw = PixelWidth(font, q, under);
        const float by = display.y - 26 * q;
        draw->AddRectFilled(ImVec2(display.x * 0.5f - uw * 0.5f - 4 * q, by - 2 * q), ImVec2(display.x * 0.5f + uw * 0.5f + 4 * q, by + 9 * q),
                            IM_COL32(0, 0, 0, 120));
        PixelText(draw, font, q, ImVec2(display.x * 0.5f - uw * 0.5f, by), under, IM_COL32(255, 255, 255, 255));

        char keys[200];
        std::snprintf(keys, sizeof(keys), "%s", Tr("Drag: move   Wheel: zoom   Space: center on you   M / Esc: close"));
        const float kw = PixelWidth(font, q, keys);
        PixelText(draw, font, q, ImVec2(display.x * 0.5f - kw * 0.5f, display.y - 12 * q), keys, IM_COL32(200, 200, 200, 230));

        char zoom[48];
        if (g_shownZoom >= 1.0f) {
            std::snprintf(zoom, sizeof(zoom), "%.1fx", g_shownZoom);
        } else {
            std::snprintf(zoom, sizeof(zoom), "1/%.0fx", 1.0f / g_shownZoom);
        }
        PixelText(draw, font, q, ImVec2(6 * q, 6 * q), zoom, IM_COL32(255, 255, 255, 230));
        char cost[48];
        std::snprintf(cost, sizeof(cost), "%.1f ms", g_fillMs);
        PixelText(draw, font, q, ImVec2(6 * q, 18 * q), cost, IM_COL32(170, 170, 170, 200));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace luma::render::worldmap
