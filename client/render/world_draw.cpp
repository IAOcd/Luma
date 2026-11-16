// =============================================================================
//  world_draw.cpp - Drawing world shapes on top of the game (see world_draw.h)
// =============================================================================
//  THE PROJECTION, step by step. For each point P of the world:
//    1. d = P - camera position (where the point is, seen from the camera).
//    2. Measure d along the camera's three directions:
//         z = how far AHEAD (forward), x = how far to the RIGHT, y = how far UP.
//       If z is negative, the point is behind you: it can't be drawn.
//    3. Divide by z (the perspective: twice as far = half as big) and
//       multiply by the game's projection scales (they say how wide the view
//       is: the FOV, and the zoom changes them). That gives a number from -1
//       to 1 across the screen ("NDC").
//    4. Turn -1..1 into pixels: x from left to right, y from top to bottom.
//
//  A shape that is partly behind the camera is CUT at a plane just in front
//  of it (the "near plane"), so only the visible part is drawn. That's what
//  the GPU does too ("clipping").
// =============================================================================
#include "render/world_draw.h"

#include <algorithm>
#include <cmath>

#include "render/music.h"
#include "sdk/camera.h"

namespace luma::render::worlddraw {

namespace {

constexpr float kNear = 0.05f;  // blocks: closer than this to the camera = cut

std::vector<sdk::draw3d::WorldShapes> g_channels[sdk::draw3d::kChannelCount];
bool g_sentToGame[sdk::draw3d::kChannelCount] = {};
int g_lastCount = 0;

struct Camera {
    sdk::camera::RenderView rv;
    float width, height;
};

bool GetCamera(Camera& c, ImVec2 display) {
    c.width = display.x;
    c.height = display.y;
    return display.x > 0.0f && display.y > 0.0f && sdk::camera::GetRenderView(c.rv);
}

// Steps 1 and 2: a world point in the camera's directions (x right, y up, z
// ahead), with the game's own view matrix (sdk/camera.h GetRenderView).
struct CamPoint {
    float x, y, z;
};
inline CamPoint ToCamera(const Camera& c, const float* p) {
    const float* m = c.rv.view;
    const float d0 = p[0] - c.rv.pos[0], d1 = p[1] - c.rv.pos[1], d2 = p[2] - c.rv.pos[2];
    return {m[0] * d0 + m[4] * d1 + m[8] * d2 + m[12], m[1] * d0 + m[5] * d1 + m[9] * d2 + m[13],
            -(m[2] * d0 + m[6] * d1 + m[10] * d2 + m[14])};  // (row 2 points backward: ahead is its opposite)
}
// Steps 3 and 4 (z must be >= kNear).
inline ImVec2 ToScreen(const Camera& c, const CamPoint& q) {
    const float inv = 1.0f / q.z;
    return ImVec2((q.x * c.rv.proj00 * inv + 1.0f) * 0.5f * c.width, (1.0f - q.y * c.rv.proj11 * inv) * 0.5f * c.height);
}
inline CamPoint Lerp(const CamPoint& a, const CamPoint& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// A filled polygon (3 or 4 points), cut at the near plane if needed.
void FillPolygon(ImDrawList* draw, const Camera& c, const CamPoint* in, int count, ImU32 color) {
    bool allInFront = true, allBehind = true;
    for (int i = 0; i < count; ++i) {
        if (in[i].z < kNear) allInFront = false; else allBehind = false;
    }
    if (allBehind) return;
    ImVec2 screen[8];
    int n = 0;
    if (allInFront) {
        for (int i = 0; i < count; ++i) screen[n++] = ToScreen(c, in[i]);
    } else {
        // Sutherland-Hodgman with one plane: keep the points in front, and
        // where an edge crosses the plane, add the crossing point.
        for (int i = 0; i < count; ++i) {
            const CamPoint& a = in[i];
            const CamPoint& b = in[(i + 1) % count];
            const bool aIn = a.z >= kNear, bIn = b.z >= kNear;
            if (aIn) screen[n++] = ToScreen(c, a);
            if (aIn != bIn) screen[n++] = ToScreen(c, Lerp(a, b, (kNear - a.z) / (b.z - a.z)));
        }
    }
    if (n < 3) return;
    // (Fan of triangles, written straight into ImGui's buffers: no
    // anti-aliasing fringe, so it's cheap and the pixel letters stay sharp.)
    draw->PrimReserve((n - 2) * 3, n);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImDrawIdx base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    for (int i = 0; i < n; ++i) draw->PrimWriteVtx(screen[i], uv, color);
    for (int i = 1; i + 1 < n; ++i) {
        draw->PrimWriteIdx(base);
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + i));
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + i + 1));
    }
}

void DrawLine(ImDrawList* draw, const Camera& c, const float* p0, const float* p1, ImU32 color) {
    CamPoint a = ToCamera(c, p0), b = ToCamera(c, p1);
    if (a.z < kNear && b.z < kNear) return;
    if (a.z < kNear) a = Lerp(a, b, (kNear - a.z) / (b.z - a.z));
    if (b.z < kNear) b = Lerp(b, a, (kNear - b.z) / (a.z - b.z));
    draw->AddLine(ToScreen(c, a), ToScreen(c, b), color, 1.0f);
}

ImU32 ToColor(const sdk::draw3d::Color& c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, std::clamp(c.a, 0.0f, 1.0f)));
}

}  // namespace

bool ByLuma() {
    return music::Get().worldDraw == 0;
}

bool Ready() {
    if (ByLuma()) {
        sdk::camera::RenderView v;
        return sdk::camera::GetRenderView(v);
    }
    return sdk::draw3d::Available();
}

void Submit(std::vector<sdk::draw3d::WorldShapes> shapes, int channel) {
    if (channel < 0 || channel >= sdk::draw3d::kChannelCount) return;
    if (ByLuma()) {
        if (g_sentToGame[channel]) {
            sdk::draw3d::SetWorldShapes({}, channel);  // (it was the game's: take them out of the world)
            g_sentToGame[channel] = false;
        }
        g_channels[channel] = std::move(shapes);
        return;
    }
    g_channels[channel].clear();
    // The game only understands triangles and lines.
    shapes.erase(std::remove_if(shapes.begin(), shapes.end(), [](const sdk::draw3d::WorldShapes& s) { return s.quads; }),
                 shapes.end());
    const bool any = !shapes.empty();
    if (any || g_sentToGame[channel]) {
        sdk::draw3d::SetWorldShapes(std::move(shapes), channel);
        g_sentToGame[channel] = any;
    }
}

void Draw(ImDrawList* draw, ImVec2 display) {
    g_lastCount = 0;
    if (!ByLuma()) return;
    bool any = false;
    for (const auto& channel : g_channels) any = any || !channel.empty();
    if (!any) return;
    Camera cam;
    if (!GetCamera(cam, display)) return;

    // Lines without anti-aliasing: the visualizer draws many thin lines side
    // by side (one per pixel), and the soft edge would only cost more.
    const ImDrawListFlags oldFlags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedLines;
    // The visualizer first (it's farther, in the sky), the lyrics on top.
    for (const int channel : {sdk::draw3d::kChannelVisualizer, sdk::draw3d::kChannelLyrics, sdk::draw3d::kChannelWaypoints}) {
        for (const sdk::draw3d::WorldShapes& shape : g_channels[channel]) {
            if (shape.color.a <= 0.005f) continue;
            const ImU32 color = ToColor(shape.color);
            const float* p = shape.triangles.data();
            const std::size_t floats = shape.triangles.size();
            if (shape.lines) {
                for (std::size_t i = 0; i + 6 <= floats; i += 6) DrawLine(draw, cam, p + i, p + i + 3, color);
                g_lastCount += static_cast<int>(floats / 6);
            } else {
                const int corners = shape.quads ? 4 : 3;
                const std::size_t step = static_cast<std::size_t>(corners) * 3;
                CamPoint q[4];
                for (std::size_t i = 0; i + step <= floats; i += step) {
                    for (int k = 0; k < corners; ++k) q[k] = ToCamera(cam, p + i + static_cast<std::size_t>(k) * 3);
                    FillPolygon(draw, cam, q, corners, color);
                }
                g_lastCount += static_cast<int>(floats / step);
            }
        }
    }
    draw->Flags = oldFlags;
}

int LastPrimitiveCount() {
    return g_lastCount;
}

}  // namespace luma::render::worlddraw
