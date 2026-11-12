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
#include <string>
#include <vector>

#include <windows.h>

#include "core/logger.h"
#include "core/tr.h"
#include "modules/world/map_look.h"
#include "modules/world/map_store.h"
#include "modules/world/radar_style.h"
#include "modules/world/waypoints.h"
#include "perf/timer.h"
#include "render/gpu.h"
#include "render/hud.h"
#include "render/input.h"
#include "render/icons.h"
#include "render/pixel_art.h"
#include "render/theme.h"
#include "sdk/blocks.h"
#include "sdk/network.h"
#include "sdk/player.h"
#include "sdk/radar.h"

namespace luma::render::worldmap {

namespace {

namespace mapstore = modules::world::mapstore;
namespace waypoints = modules::world::waypoints;
namespace maplook = modules::world::maplook;

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
Settings g_settings;
bool g_settingsChanged = false;

// The map's windows (only one at a time).
enum class Window { None, Context, Edit };
MinimapPanel g_minimapPanel = nullptr;
float g_scroll[5] = {0, 0, 0, 0, 0};  // each sidebar page's scroll
char g_search[32] = "";
// Where the map was when it closed (to open there again, in the same world).
bool g_haveLast = false;
double g_lastX = 0, g_lastZ = 0;
float g_lastZoom = 2.0f;
std::string g_lastWorld;
Window g_window = Window::None;
ImVec2 g_contextAt;              // where the right-click menu opened (screen)
int g_contextX = 0, g_contextZ = 0;  // and the block under it
int g_editing = -1;              // the waypoint being edited (index in the list)
char g_editX[16], g_editY[16], g_editZ[16];
ImVec2 g_pressAt;                // where the left button went down (a click or a drag?)

namespace px = render::pixel;

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
    const std::uint32_t empty = maplook::Background();
    const bool plain = maplook::Plain();
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
            row[x] = v == 0 ? empty : plain ? v : maplook::Grade(v);
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

Settings& Config() {
    return g_settings;
}

void SetMinimapPanel(MinimapPanel panel) {
    g_minimapPanel = panel;
}

bool TakeSettingsChanged() {
    const bool c = g_settingsChanged;
    g_settingsChanged = false;
    return c;
}

bool IsActive() {
    return g_active;
}

bool Escape() {
    if (g_window == Window::None) return false;
    g_window = Window::None;
    g_editing = -1;
    return true;
}

void Open() {
    sdk::player::Snapshot p;
    const std::string world = waypoints::StorageName() + "/" + std::to_string(waypoints::Dimension());
    if (g_settings.remember && g_haveLast && g_lastWorld == world) {
        g_centerX = g_lastX;
        g_centerZ = g_lastZ;
        g_zoom = g_lastZoom;
    } else {
        if (sdk::player::Read(p)) {
            g_centerX = p.feet[0];
            g_centerZ = p.feet[2];
        }
        g_zoom = std::clamp(g_settings.startZoom, kMinZoom, kMaxZoom);
    }
    g_shownZoom = g_zoom;
    g_drawnZoom = -1.0f;  // (fill it again)
    g_active = true;
}

void Close() {
    g_active = false;
    g_haveLast = true;
    g_lastX = g_centerX;
    g_lastZ = g_centerZ;
    g_lastZoom = g_zoom;
    g_lastWorld = waypoints::StorageName() + "/" + std::to_string(waypoints::Dimension());
    g_window = Window::None;
    g_editing = -1;
    waypoints::MarkDirty();  // (whatever was edited gets saved)
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
    ImGui::SetNextItemAllowOverlap();  // (the map's windows and buttons, drawn later, take the mouse first)
    ImGui::InvisibleButton("##map", display, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool mapActive = ImGui::IsItemActive();
    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool leftPressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool leftReleased = ImGui::IsItemDeactivated();
    if (leftPressed) g_pressAt = io.MousePos;

    // --- Moving and zooming ---
    if (mapActive && ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        g_centerX -= io.MouseDelta.x / g_shownZoom;
        g_centerZ -= io.MouseDelta.y / g_shownZoom;
    }
    const ImVec2 mouse = io.MousePos;
    auto worldAt = [&](ImVec2 s, float zoom, double& wx, double& wz) {
        wx = g_centerX + (s.x - display.x * 0.5f) / zoom;
        wz = g_centerZ + (s.y - display.y * 0.5f) / zoom;
    };
    if (hovered && io.MouseWheel != 0.0f) {
        g_zoom = std::clamp(g_zoom * std::pow(2.0f, io.MouseWheel * 0.5f * g_settings.zoomSpeed), kMinZoom, kMaxZoom);
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
    const bool typing = io.WantTextInput;
    if ((g_settings.follow || (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !typing)) && havePlayer && !mapActive) {
        g_centerX = p.feet[0];
        g_centerZ = p.feet[2];
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !typing) {
        input::RequestMenuToggle();  // (closing the menu closes the map: menu::OnClosed)
    }

    // --- The picture: filled again only when the view or the blocks changed ---
    {
        // What you see and a margin around it (so dragging finds it ready),
        // read from disk for up to 3 ms a frame, the middle first.
        const double halfW = w * 0.75 / g_shownZoom, halfH = h * 0.75 / g_shownZoom;
        mapstore::Prefetch(static_cast<int>(std::floor(g_centerX - halfW)), static_cast<int>(std::floor(g_centerZ - halfH)),
                           static_cast<int>(std::floor(g_centerX + halfW)), static_cast<int>(std::floor(g_centerZ + halfH)), 64);
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
    const bool fresh = mapstore::Version() + maplook::Version() * 7919u != g_drawnVersion;
    const bool interacting = mapActive || std::fabs(g_shownZoom - g_zoom) > 0.0001f;
    const double now = ImGui::GetTime();
    // Does the picture still cover the whole screen?
    bool covers = false;
    if (g_texture != ImTextureID{} && g_drawnZoom > 0.0f) {
        const double halfW = tw * 0.5 / g_drawnZoom, halfH = th * 0.5 / g_drawnZoom;
        const double viewW = w * 0.5 / g_shownZoom, viewH = h * 0.5 / g_shownZoom;
        covers = g_centerX - viewW >= g_drawnX - halfW && g_centerX + viewW <= g_drawnX + halfW && g_centerZ - viewH >= g_drawnZ - halfH &&
                 g_centerZ + viewH <= g_drawnZ + halfH && g_shownZoom <= g_drawnZoom * 1.6f;
    }
    // (New data alone, like regions arriving from disk: at most 5 times a second.)
    const bool due = interacting ? (!covers && now - g_lastFill > 0.12) : (moved || (fresh && now - g_lastFill > 0.2));
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
        g_drawnVersion = mapstore::Version() + maplook::Version() * 7919u;
    }
    draw->AddRectFilled(ImVec2(0, 0), display, maplook::Background());
    if (g_texture != ImTextureID{} && g_texW == tw && g_texH == th) {
        // Where the picture's view sits in the current view (to the fraction
        // of a pixel while moving: smooth; on whole pixels when still: crisp).
        const float k = g_shownZoom / g_drawnZoom;
        const float x0 = static_cast<float>((g_drawnX - tw * 0.5 / g_drawnZoom - g_centerX) * g_shownZoom + display.x * 0.5);
        const float y0 = static_cast<float>((g_drawnZ - th * 0.5 / g_drawnZoom - g_centerZ) * g_shownZoom + display.y * 0.5);
        const ImVec2 a = interacting ? ImVec2(x0, y0) : ImVec2(std::round(x0), std::round(y0));
        draw->AddImage(g_texture, a, ImVec2(a.x + static_cast<float>(tw) * k, a.y + static_cast<float>(th) * k));
    }

    auto toScreen = [&](double wx, double wz) {
        return ImVec2(static_cast<float>((wx - g_centerX) * g_shownZoom + display.x * 0.5f),
                      static_cast<float>((wz - g_centerZ) * g_shownZoom + display.y * 0.5f));
    };
    const int qi = static_cast<int>(q);
    const float sideW = g_settings.sidebar ? std::floor(150 * q) : 0.0f;  // the sidebar's width (0 = closed)
    const px::PixelText text = px::TextAt(qi);
    const px::PixelText smallText = px::TextAt(std::max(1, qi - 1));

    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(theme::Accent(1.0f));
    // --- Grids: chunks (every 16 blocks, only zoomed in, or it'd be a gray
    // mess) and regions (every 512 blocks, in the accent color) ---
    auto grid = [&](double step, ImU32 line, float thick) {
        const double x0 = g_centerX - display.x * 0.5 / g_shownZoom, z0 = g_centerZ - display.y * 0.5 / g_shownZoom;
        for (double x = std::floor(x0 / step) * step; x < x0 + display.x / g_shownZoom + step; x += step) {
            const float sx = std::floor(toScreen(x, 0).x);
            draw->AddRectFilled(ImVec2(sx, 0), ImVec2(sx + thick, display.y), line);
        }
        for (double z = std::floor(z0 / step) * step; z < z0 + display.y / g_shownZoom + step; z += step) {
            const float sy = std::floor(toScreen(0, z).y);
            draw->AddRectFilled(ImVec2(0, sy), ImVec2(display.x, sy + thick), line);
        }
    };
    if (g_settings.chunkGrid && g_shownZoom >= 1.5f) grid(16.0, IM_COL32(0, 0, 0, 55), 1.0f);
    if (g_settings.regionGrid && g_shownZoom >= 0.12f) {
        grid(512.0, (accent & 0x00FFFFFFu) | (140u << 24), std::max(1.0f, q * 0.5f));
    }

    // --- Waypoints: the same banners as the minimap (click one to edit it) ---
    const int dim = waypoints::Dimension();
    auto& list = waypoints::List();
    int clickedWaypoint = -1;
    const bool wasClick = leftReleased && std::fabs(io.MousePos.x - g_pressAt.x) < 4.0f && std::fabs(io.MousePos.y - g_pressAt.y) < 4.0f;
    if (g_settings.waypoints) {
        int named = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const waypoints::Waypoint& wp = list[i];
            if (dim >= 0 && wp.dimension != dim) continue;
            if (wp.death && !g_settings.deathPoint) continue;
            const ImVec2 at = toScreen(wp.x + 0.5, wp.z + 0.5);
            if (at.x < -20 * q || at.x > display.x + 20 * q || at.y < -10 * q || at.y > display.y + 30 * q) continue;
            const ImVec2 tip(std::floor(at.x), std::floor(at.y));
            const bool editing = g_window == Window::Edit && g_editing == static_cast<int>(i);
            // Under the mouse (or being edited): a pixel bigger, like in the world.
            const bool over = io.MousePos.x >= tip.x - 4 * q && io.MousePos.x <= tip.x + 5 * q && io.MousePos.y >= tip.y - 11 * q &&
                              io.MousePos.y <= tip.y + q && hovered;
            const int bq = std::min(8, qi + g_settings.bannerSize);
            const px::PixelText& t = (over || editing) ? px::TextAt(std::min(8, bq + 1)) : px::TextAt(bq);
            px::Banner(draw, tip, t, wp.color, wp.visible ? 255 : 150, wp.name.data());
            if (g_settings.names && (named < 40 || over) && wp.name[0] != 0) {
                ++named;
                px::NameTag(draw, tip.x + 0.5f * t.q, tip.y - 11.0f * t.q, over ? text : smallText, wp.name.data(), IM_COL32(255, 255, 255, 235),
                            -1.0e6f, 1.0e6f);
            }
            if (over && wasClick) clickedWaypoint = static_cast<int>(i);
        }
    }
    auto openEdit = [&](int index) {
        g_window = Window::Edit;
        g_editing = index;
        const waypoints::Waypoint& wp = list[static_cast<std::size_t>(index)];
        std::snprintf(g_editX, sizeof(g_editX), "%d", wp.x);
        std::snprintf(g_editY, sizeof(g_editY), "%d", wp.y);
        std::snprintf(g_editZ, sizeof(g_editZ), "%d", wp.z);
    };
    if (clickedWaypoint >= 0) openEdit(clickedWaypoint);
    else if (wasClick && g_window != Window::None && g_window != Window::Edit) g_window = Window::None;  // (a click on the map closes a menu)

    // --- The entities you have loaded around you (LOCAL WORLDS ONLY) ---
    if (g_settings.entities && havePlayer) {
        static std::vector<sdk::radar::Entity> list2;  // (kept between frames)
        sdk::radar::Copy(list2);
        for (const sdk::radar::Entity& e : list2) {
            const ImVec2 at = toScreen(e.x, e.z);
            if (at.x < 0 || at.y < 0 || at.x > display.x || at.y > display.y) continue;
            const auto st = modules::world::radarstyle::Of(e.kind);
            px::Dot(draw, at, std::max(1.0f, std::round(q * 0.75f)), modules::world::radarstyle::ByHeight(st.color, e.y - p.feet[1]), st.shape,
                    st.size);
        }
    }

    // --- You (on the edge if you're off the screen) ---
    if (havePlayer) {
        ImVec2 at = toScreen(p.feet[0], p.feet[2]);
        at.x = std::clamp(at.x, 10.0f * q, display.x - 10.0f * q);
        at.y = std::clamp(at.y, 10.0f * q, display.y - 10.0f * q);
        DrawPlayer(draw, at, p.yaw, q);
    }

    // --- Right click: a small menu at the cursor ---
    if (rightClicked) {
        double wx, wz;
        worldAt(io.MousePos, g_shownZoom, wx, wz);
        g_contextX = static_cast<int>(std::floor(wx));
        g_contextZ = static_cast<int>(std::floor(wz));
        g_contextAt = io.MousePos;
        g_window = Window::Context;
    }

    // --- Rulers: the coordinates along the top and right edges ---
    if (g_settings.rulers) {
        // A step that leaves about 90 screen pixels between numbers.
        double step = 16.0;
        while (step * g_shownZoom < 90.0 * q / 2.0 && step < 65536.0) step *= 2.0;
        const double x0 = g_centerX - display.x * 0.5 / g_shownZoom, z0 = g_centerZ - display.y * 0.5 / g_shownZoom;
        const ImU32 tick = IM_COL32(255, 255, 255, 120);
        draw->AddRectFilled(ImVec2(sideW, 0), ImVec2(display.x, 9 * q), IM_COL32(0, 0, 0, 90));
        draw->AddRectFilled(ImVec2(display.x - 9 * q, 9 * q), ImVec2(display.x, display.y), IM_COL32(0, 0, 0, 90));
        char num[24];
        for (double x = std::floor(x0 / step) * step; x < x0 + display.x / g_shownZoom; x += step) {
            const float sx = std::floor(toScreen(x, 0).x);
            if (sx < sideW + 4 * q || sx > display.x - 30 * q) continue;
            draw->AddRectFilled(ImVec2(sx, 0), ImVec2(sx + std::max(1.0f, q * 0.5f), 3 * q), tick);
            std::snprintf(num, sizeof(num), "%d", static_cast<int>(x));
            px::Text(draw, smallText, ImVec2(sx + 2 * q, 2 * q), num, IM_COL32(230, 230, 230, 230), false);
        }
        for (double z = std::floor(z0 / step) * step; z < z0 + display.y / g_shownZoom; z += step) {
            const float sy = std::floor(toScreen(0, z).y);
            if (sy < 14 * q || sy > display.y - 40 * q) continue;
            draw->AddRectFilled(ImVec2(display.x - 3 * q, sy), ImVec2(display.x, sy + std::max(1.0f, q * 0.5f)), tick);
            std::snprintf(num, sizeof(num), "%d", static_cast<int>(z));
            const float numW = px::TextWidth(smallText, num);
            px::Text(draw, smallText, ImVec2(display.x - 4 * q - numW, sy + 2 * q), num, IM_COL32(230, 230, 230, 230), false);
        }
    }

    // --- Texts: the world on top; the block under the cursor and the keys at the bottom ---
    const float midX = sideW + (display.x - sideW) * 0.5f;
    {
        static const char* const kDims[] = {"Overworld", "Nether", "End"};
        char title[200];
        std::snprintf(title, sizeof(title), "%s  -  %s", waypoints::HaveWorld() ? waypoints::WorldLabel().c_str() : Tr("World map"),
                      Tr(kDims[std::clamp(dim, 0, 2)]));
        const float titleW = px::TextWidth(text, title);
        const float ty = g_settings.rulers ? 13 * q : 5 * q;
        px::DarkPanel(draw, ImVec2(midX - titleW * 0.5f - 5 * q, ty), ImVec2(midX + titleW * 0.5f + 5 * q, ty + 13 * q), q);
        draw->AddRectFilled(ImVec2(midX - titleW * 0.5f - 5 * q, ty + 12 * q), ImVec2(midX + titleW * 0.5f + 5 * q, ty + 13 * q), accent);
        px::Text(draw, text, ImVec2(midX - titleW * 0.5f, ty + 3 * q), title, IM_COL32(255, 255, 255, 255));

        if (g_settings.coords && hovered) {
            double mx, mz;
            worldAt(io.MousePos, g_shownZoom, mx, mz);
            char under[96];
            std::snprintf(under, sizeof(under), "X: %d   Z: %d", static_cast<int>(std::floor(mx)), static_cast<int>(std::floor(mz)));
            const float uw = px::TextWidth(text, under);
            const float by = display.y - 30 * q;
            px::DarkPanel(draw, ImVec2(midX - uw * 0.5f - 5 * q, by - 3 * q), ImVec2(midX + uw * 0.5f + 5 * q, by + 10 * q), q);
            px::Text(draw, text, ImVec2(midX - uw * 0.5f, by), under, IM_COL32(255, 255, 255, 255));
        }
        const char* keys = Tr("Drag: move   Wheel: zoom   Space: center on you   Right click: new waypoint   M / Esc: close");
        const float kw = px::TextWidth(smallText, keys);
        px::Text(draw, smallText, ImVec2(midX - kw * 0.5f, display.y - 13 * q), keys, IM_COL32(200, 200, 200, 230));

        char zoom[48];
        if (g_shownZoom >= 1.0f) {
            std::snprintf(zoom, sizeof(zoom), "%.1fx", g_shownZoom);
        } else {
            std::snprintf(zoom, sizeof(zoom), "1/%.0fx", 1.0f / g_shownZoom);
        }
        const float zx = display.x - (g_settings.rulers ? 13 : 5) * q - px::TextWidth(text, zoom);
        px::Text(draw, text, ImVec2(zx, (g_settings.rulers ? 14 : 6) * q), zoom, IM_COL32(255, 255, 255, 230));
        if (g_settings.cost) {
            char cost[48];
            std::snprintf(cost, sizeof(cost), "%.1f ms", g_fillMs);
            px::Text(draw, smallText, ImVec2(display.x - (g_settings.rulers ? 13 : 5) * q - px::TextWidth(smallText, cost), (g_settings.rulers ? 26 : 18) * q),
                     cost, IM_COL32(170, 170, 170, 200));
        }
    }

    // A panel's background takes the mouse (so the map behind doesn't move).
    auto blocker = [&](const char* id, ImVec2 a0, ImVec2 b0) {
        ImGui::SetCursorScreenPos(a0);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton(id, ImVec2(b0.x - a0.x, b0.y - a0.y));
    };
    // A dark see-through window with the accent along its top.
    auto window = [&](ImVec2 a0, ImVec2 b0) {
        draw->AddRectFilled(a0, b0, IM_COL32(12, 12, 16, 232));
        draw->AddRect(a0, b0, IM_COL32(255, 255, 255, 30), 0.0f, 0, q);
        draw->AddRectFilled(a0, ImVec2(b0.x, a0.y + 2 * q), accent);
    };
    auto form = [&](float x, float y, float width) {
        px::Form f;
        f.draw = draw;
        f.text = text;
        f.hint = smallText;
        f.x = x;
        f.y = y;
        f.width = width;
        f.accent = accent;
        return f;
    };
    bool changed = false;

    // ============================ THE SIDEBAR ============================
    if (g_settings.sidebar) {
        const ImVec2 a0(0, 0), b0(sideW, display.y);
        blocker("##wm_side_bg", a0, b0);
        draw->AddRectFilled(a0, b0, IM_COL32(10, 10, 14, 215));
        draw->AddRectFilled(ImVec2(b0.x - q, 0), ImVec2(b0.x, display.y), accent);
        const float pad = 8 * q;
        // Title
        px::Text(draw, text, ImVec2(pad, 8 * q), Tr("World map"), IM_COL32(255, 255, 255, 255));
        if (waypoints::HaveWorld()) px::Text(draw, smallText, ImVec2(pad, 20 * q), waypoints::WorldLabel().c_str(), IM_COL32(160, 160, 166, 255), false);
        // Close the sidebar: a small "<" on its top-right.
        if (px::Button("##wm_side_close", "<", ImVec2(sideW - pad - 12 * q, 6 * q), ImVec2(12 * q, 12 * q), text)) {
            g_settings.sidebar = false;
            changed = true;
        }
        // The pages: five icons.
        static const char* const kTabs[] = {"Map", "Minimap", "Waypoints", "Terrain", "View"};
        const char* const tabIcons[] = {icons::kCompass, icons::kCrosshair, icons::kMapPin, icons::kSun, icons::kGrid};
        const float tabW = std::floor((sideW - 2 * pad - 4 * 2 * q) / 5.0f), tabH = 18 * q, ty = 30 * q;
        for (int i = 0; i < 5; ++i) {
            const ImVec2 ta(pad + static_cast<float>(i) * (tabW + 2 * q), ty);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(ta);
            if (ImGui::InvisibleButton("##tab", ImVec2(tabW, tabH)) && g_settings.tab != i) {
                g_settings.tab = i;
                changed = true;
            }
            const bool over = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = g_settings.tab == i;
            draw->AddRectFilled(ta, ImVec2(ta.x + tabW, ta.y + tabH), IM_COL32(0, 0, 0, 255));
            draw->AddRectFilled(ImVec2(ta.x + q, ta.y + q), ImVec2(ta.x + tabW - q, ta.y + tabH - q),
                                on ? accent : over ? IM_COL32(70, 70, 78, 255) : IM_COL32(40, 40, 48, 255));
            draw->AddRectFilled(ImVec2(ta.x + q, ta.y + tabH - 2 * q), ImVec2(ta.x + tabW - q, ta.y + tabH - q), IM_COL32(0, 0, 0, 90));
            icons::Draw(draw, tabIcons[i], ImVec2(ta.x + tabW * 0.5f, ta.y + tabH * 0.5f - 0.5f * q), 10 * q,
                        on ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 206, 255));
            if (over) ImGui::SetTooltip("%s", Tr(kTabs[i]));
        }
        // The page, scrolled with the wheel (only inside the sidebar).
        const int tab = std::clamp(g_settings.tab, 0, 4);
        const float top = ty + tabH + 4 * q, bottom = display.y - 6 * q;
        const bool overSide = io.MousePos.x < sideW && io.MousePos.y > top;
        if (overSide && io.MouseWheel != 0.0f) g_scroll[tab] = std::max(0.0f, g_scroll[tab] - io.MouseWheel * 24 * q);
        draw->PushClipRect(ImVec2(0, top), ImVec2(sideW, bottom), true);
        px::Form f = form(pad, top - g_scroll[tab], sideW - 2 * pad);
        f.clipTop = top;
        f.clipBottom = bottom;
        f.Section(Tr(kTabs[tab]));
        maplook::Look& look = maplook::Get();
        bool lookChanged = false;
        if (tab == 0) {
            // ---- Map ----
            changed |= f.Toggle(Tr("Waypoints"), &g_settings.waypoints);
            changed |= f.Toggle(Tr("Their names"), &g_settings.names);
            changed |= f.Toggle(Tr("Death point"), &g_settings.deathPoint);
            changed |= f.Toggle(Tr("Entities (own worlds)"), &g_settings.entities);
            changed |= f.SliderInt(Tr("Banner size"), &g_settings.bannerSize, 0, 2, "+%d");
            f.Section(Tr("While open"));
            changed |= f.Toggle(Tr("Follow me"), &g_settings.follow);
            changed |= f.Toggle(Tr("Coordinates under the cursor"), &g_settings.coords);
            changed |= f.Toggle(Tr("Show its cost"), &g_settings.cost);
            lookChanged |= f.Colors(Tr("Unexplored background"), look.background);
            if (f.ButtonRow(Tr("Center on you"), true) && havePlayer) {
                g_centerX = p.feet[0];
                g_centerZ = p.feet[2];
            }
        } else if (tab == 1) {
            // ---- Minimap: drawn by the minimap module ----
            if (g_minimapPanel != nullptr) {
                changed |= g_minimapPanel(f);
            } else {
                f.Hint(Tr("Turn on the Minimap module (Luma's menu, World) to set it up here."));
            }
        } else if (tab == 2) {
            // ---- Waypoints: search, add, the list ----
            f.Input("##wm_search", g_search, sizeof(g_search), Tr("Search..."));
            if (f.ButtonRow(Tr("New waypoint where you are"), true) && havePlayer && waypoints::HaveWorld()) {
                const std::size_t i = waypoints::Add(static_cast<int>(std::floor(p.feet[0])), static_cast<int>(std::floor(p.feet[1])),
                                                     static_cast<int>(std::floor(p.feet[2])));
                openEdit(static_cast<int>(i));
            }
            std::string needle = g_search;
            for (char& ch : needle) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            int shown = 0;
            const float rowH = 14 * q;
            for (std::size_t i = 0; i < list.size(); ++i) {
                const waypoints::Waypoint& wp = list[i];
                if (dim >= 0 && wp.dimension != dim) continue;
                if (!needle.empty()) {
                    std::string name = wp.name.data();
                    for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (name.find(needle) == std::string::npos) continue;
                }
                ++shown;
                if (f.Visible(rowH)) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::SetCursorScreenPos(ImVec2(f.x, f.y));
                    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(f.width - 16 * q, rowH - q));
                    const bool rowOver = ImGui::IsItemHovered();
                    ImGui::SetCursorScreenPos(ImVec2(f.x + f.width - 14 * q, f.y));
                    const bool editClicked = ImGui::InvisibleButton("##edit", ImVec2(14 * q, rowH - q));
                    const bool editOver = ImGui::IsItemHovered();
                    ImGui::PopID();
                    if (rowOver || editOver) draw->AddRectFilled(ImVec2(f.x - 3 * q, f.y), ImVec2(f.x + f.width + 3 * q, f.y + rowH - q), IM_COL32(255, 255, 255, 22));
                    draw->AddRectFilled(ImVec2(f.x, f.y + 3 * q), ImVec2(f.x + 8 * q, f.y + 11 * q), IM_COL32(0, 0, 0, 255));
                    draw->AddRectFilled(ImVec2(f.x + q, f.y + 4 * q), ImVec2(f.x + 7 * q, f.y + 10 * q), px::Shade(wp.color, 1.0f, 255));
                    px::Text(draw, text, ImVec2(f.x + 12 * q, f.y + (rowH - q - text.capHeight) * 0.5f), wp.name.data(),
                             rowOver ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 224, 255));
                    if (havePlayer) {
                        const double ddx = wp.x + 0.5 - p.feet[0], ddz = wp.z + 0.5 - p.feet[2];
                        char d[24];
                        std::snprintf(d, sizeof(d), "%.0f m", std::sqrt(ddx * ddx + ddz * ddz));
                        px::Text(draw, smallText, ImVec2(f.x + f.width - 18 * q - px::TextWidth(smallText, d), f.y + (rowH - q - smallText.capHeight) * 0.5f),
                                 d, IM_COL32(150, 150, 156, 255), false);
                    }
                    icons::Draw(draw, icons::kSettings, ImVec2(f.x + f.width - 7 * q, f.y + rowH * 0.5f), 9 * q,
                                editOver ? accent : IM_COL32(170, 170, 176, 255));
                    if (clicked) {
                        g_centerX = wp.x + 0.5;
                        g_centerZ = wp.z + 0.5;
                    }
                    if (editClicked) openEdit(static_cast<int>(i));
                }
                f.y += rowH;
            }
            if (shown == 0) f.Hint(needle.empty() ? Tr("No waypoints here yet. Right click the map to add one.") : Tr("Nothing matches that name."));
        } else if (tab == 3) {
            // ---- Terrain ----
            lookChanged |= f.Slider(Tr("Brightness"), &look.brightness, 0.5f, 1.5f, "%.2f");
            lookChanged |= f.Slider(Tr("Contrast"), &look.contrast, 0.5f, 1.5f, "%.2f");
            lookChanged |= f.Slider(Tr("Saturation"), &look.saturation, 0.0f, 2.0f, "%.2f");
            f.Section(Tr("Painting"));
            lookChanged |= f.Slider(Tr("Relief"), &look.relief, 0.0f, 2.0f, "%.2f");
            lookChanged |= f.Toggle(Tr("Lighter on high ground"), &look.heightTint);
            lookChanged |= f.Toggle(Tr("Water by depth"), &look.waterDepth);
            lookChanged |= f.Toggle(Tr("Dark shores"), &look.shores);
            lookChanged |= f.Toggle(Tr("Block texture"), &look.texture);
            lookChanged |= f.Toggle(Tr("Biome colors"), &look.biomes);
            if (!sdk::blocks::BiomeTintAvailable()) f.Hint(Tr("(Not available in this game version: plains colors.)"));
            f.Hint(Tr("Brightness, contrast and saturation change everything at once. Relief, water, shores and texture repaint the minimap now; the world map shows them where you walk again."));
            if (f.ButtonRow(Tr("Reset the look"))) {
                const float bg[3] = {look.background[0], look.background[1], look.background[2]};
                look = maplook::Look{};
                std::copy(bg, bg + 3, look.background);
                lookChanged = true;
            }
        } else {
            // ---- View ----
            changed |= f.Toggle(Tr("Chunk grid"), &g_settings.chunkGrid);
            changed |= f.Toggle(Tr("Region grid"), &g_settings.regionGrid);
            changed |= f.Toggle(Tr("Coordinates on the edges"), &g_settings.rulers);
            f.Section(Tr("Zoom"));
            changed |= f.Slider(Tr("Zoom speed"), &g_settings.zoomSpeed, 0.25f, 2.0f, "%.2fx");
            changed |= f.Slider(Tr("Zoom when it opens"), &g_settings.startZoom, 0.25f, 8.0f, "%.1fx");
            changed |= f.Toggle(Tr("Open where I left it"), &g_settings.remember);
        }
        if (lookChanged) {
            maplook::Changed();
            changed = true;
        }
        const float contentH = f.y + g_scroll[tab] - top;
        g_scroll[tab] = std::clamp(g_scroll[tab], 0.0f, std::max(0.0f, contentH - (bottom - top)));
        draw->PopClipRect();
    } else {
        // Closed: a small tab on the left edge opens it again.
        if (px::Button("##wm_side_open", ">", ImVec2(0, 40 * q), ImVec2(14 * q, 24 * q), text)) {
            g_settings.sidebar = true;
            changed = true;
        }
    }

    // ============================ RIGHT-CLICK MENU ============================
    if (g_window == Window::Context) {
        const float cw = 110 * q, ch = 54 * q;
        const ImVec2 a0(std::min(g_contextAt.x, display.x - cw - 4 * q), std::min(g_contextAt.y, display.y - ch - 4 * q));
        const ImVec2 b0(a0.x + cw, a0.y + ch);
        blocker("##wm_ctx_bg", a0, b0);
        window(a0, b0);
        char where[48];
        std::snprintf(where, sizeof(where), "X %d   Z %d", g_contextX, g_contextZ);
        px::Text(draw, smallText, ImVec2(a0.x + 6 * q, a0.y + 7 * q), where, IM_COL32(170, 170, 176, 255), false);
        px::Form f = form(a0.x + 5 * q, a0.y + 16 * q, cw - 10 * q);
        if (f.ButtonRow(Tr("New waypoint"), true)) {
            if (waypoints::HaveWorld()) {
                const int y = havePlayer ? static_cast<int>(std::floor(p.feet[1])) : 64;  // (the map doesn't know the height: yours)
                const std::size_t i = waypoints::Add(g_contextX, y, g_contextZ);
                openEdit(static_cast<int>(i));
            } else {
                g_window = Window::None;
            }
        }
        if (f.ButtonRow(Tr("Center here"))) {
            g_centerX = g_contextX + 0.5;
            g_centerZ = g_contextZ + 0.5;
            g_window = Window::None;
        }
    }

    // ============================ EDITING A WAYPOINT ============================
    if (g_window == Window::Edit && g_editing >= 0 && g_editing < static_cast<int>(list.size())) {
        waypoints::Waypoint& wp = list[static_cast<std::size_t>(g_editing)];
        const ImVec2 size(170 * q, 176 * q);
        const ImVec2 a0(std::floor(midX - size.x * 0.5f), std::floor((display.y - size.y) * 0.5f));
        const ImVec2 b0(a0.x + size.x, a0.y + size.y);
        blocker("##wm_edit_bg", a0, b0);
        window(a0, b0);
        px::Text(draw, text, ImVec2(a0.x + 8 * q, a0.y + 8 * q), Tr("Waypoint"), IM_COL32(255, 255, 255, 255));
        px::Banner(draw, ImVec2(b0.x - 14 * q, a0.y + 18 * q), px::TextAt(std::min(8, qi + 1)), wp.color, 255, wp.name.data());
        px::Form f = form(a0.x + 8 * q, a0.y + 22 * q, size.x - 16 * q);
        bool edited = f.Input("##ed_name", wp.name.data(), static_cast<int>(wp.name.size()), Tr("Name"));
        // X, Y, Z side by side.
        const float fw = std::floor((f.width - 2 * 4 * q) / 3.0f);
        char* fields[3] = {g_editX, g_editY, g_editZ};
        const char* labels[3] = {"X", "Y", "Z"};
        for (int k = 0; k < 3; ++k) {
            const float fx = f.x + static_cast<float>(k) * (fw + 4 * q);
            px::Text(draw, smallText, ImVec2(fx, f.y), labels[k], IM_COL32(170, 170, 176, 255), false);
            char id[16];
            std::snprintf(id, sizeof(id), "##ed_%s", labels[k]);
            if (px::Field(id, fields[k], 16, ImVec2(fx, f.y + 7 * q), fw, text)) {
                const int v = std::atoi(fields[k]);
                if (k == 0) wp.x = v;
                if (k == 1) wp.y = std::clamp(v, -64, 320);
                if (k == 2) wp.z = v;
                edited = true;
            }
        }
        f.y += 7 * q + text.capHeight + 11 * q;
        edited |= f.Colors(Tr("Color"), wp.color);
        edited |= f.Toggle(Tr("On the minimap"), &wp.visible);
        edited |= f.Toggle(Tr("In the world"), &wp.inWorld);
        if (edited) waypoints::MarkDirty();
        const float half = std::floor((f.width - 4 * q) / 2.0f);
        px::Form left = form(f.x, b0.y - 20 * q, half), right = form(f.x + half + 4 * q, b0.y - 20 * q, half);
        if (left.ButtonRow(Tr("Done"), true)) {
            g_window = Window::None;
            g_editing = -1;
            waypoints::MarkDirty();
        }
        if (right.ButtonRow(Tr("Remove"))) {
            waypoints::Remove(static_cast<std::size_t>(g_editing));
            g_window = Window::None;
            g_editing = -1;
        }
    }
    if (changed) g_settingsChanged = true;  // (the minimap module saves these with its settings)
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace luma::render::worldmap
