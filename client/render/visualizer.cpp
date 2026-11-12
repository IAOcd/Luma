// =============================================================================
//  visualizer.cpp - The music visualizer in the sky (see visualizer.h)
// =============================================================================
//  LAYERS: every style is drawn in a few layers, back to front:
//    - a soft "glow" layer (wider, lighter, see-through) with the smoothed
//      levels: it breathes slowly;
//    - the main layer with the QUICK levels: it follows every hit;
//    - "peaks": thin caps at the highest recent value, that wait and fall.
//  HITS: when a strong hit comes (a drum, a bass drop), everything jumps a
//  little taller and brighter for a moment ("kick", core/audio_viz.h).
// =============================================================================
#include "render/visualizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/audio_viz.h"
#include "core/media.h"
#include "render/music.h"
#include "render/rainbow.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"

namespace luma::render::visualizer {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kBuckets = 20;  // colors per layer (each one is one draw call)

enum Style { kBars, kMirror, kWave, kAurora, kRings, kPillars, kLayeredWave };

bool g_sent = false;

// A value of a band array at place i of n around the ring. The spectrum goes
// from the bass to the treble and back (mirrored), so the ring closes
// without a jump.
float Around(const float* bands, int i, int n) {
    const float t = static_cast<float>(i) / static_cast<float>(n);
    const float fold = 1.0f - std::fabs(t * 2.0f - 1.0f);
    const float band = fold * static_cast<float>(audioviz::kBands - 1);
    const int b0 = static_cast<int>(band);
    const int b1 = std::min(b0 + 1, audioviz::kBands - 1);
    const float k = band - static_cast<float>(b0);
    return bands[b0] + (bands[b1] - bands[b0]) * k;
}

// The album's palette, read once per frame (Draw fills it).
float g_palette[5][3] = {{1, 1, 1}};
int g_paletteCount = 1;

// A color of the palette at "u" (0..1, it loops): smoothly from one to the next.
void PaletteAt(float u, float out[3]) {
    const int n = std::max(1, g_paletteCount);
    u = u - std::floor(u);
    const float f = u * static_cast<float>(n);
    const int a = static_cast<int>(f) % n, b = (a + 1) % n;
    const float k = f - std::floor(f);
    const float e = k * k * (3.0f - 2.0f * k);
    for (int c = 0; c < 3; ++c) out[c] = g_palette[a][c] + (g_palette[b][c] - g_palette[a][c]) * e;
}

// The color for a place on the ring (t: 0..1) and a height (level 0..1).
void Color(const music::Settings& s, float t, float level, double now, float out[3]) {
    float album[3] = {s.vizColor[0], s.vizColor[1], s.vizColor[2]};
    music::AlbumColor(album);
    switch (s.vizColorMode) {
        case 5:  // the album's palette, around the ring
            PaletteAt(t, out);
            return;
        case 6:  // the whole ring takes the album's colors one after another (8 s each)
            PaletteAt(static_cast<float>(std::fmod(now / 8.0, 1.0e6)) / static_cast<float>(std::max(1, g_paletteCount)) * 1.0f, out);
            return;
        case 7:  // from the album's color to your color, around the ring
            for (int k = 0; k < 3; ++k) {
                const float f = 1.0f - std::fabs(t * 2.0f - 1.0f);
                out[k] = album[k] + (s.vizColor[k] - album[k]) * f;
            }
            return;
        case 1: std::memcpy(out, album, sizeof(album)); return;
        case 2:  // the album's color, lighter the higher it goes
            for (int k = 0; k < 3; ++k) out[k] = album[k] + (1.0f - album[k]) * 0.75f * level;
            return;
        case 3:  // two colors, around the ring
            for (int k = 0; k < 3; ++k) {
                const float f = 1.0f - std::fabs(t * 2.0f - 1.0f);
                out[k] = s.vizColor[k] + (s.vizColor2[k] - s.vizColor[k]) * f;
            }
            return;
        case 4: {  // rainbow around the ring, slowly turning
            const double turns = static_cast<double>(t) + now * 0.05;
            rainbow::Hsv(static_cast<float>(turns - std::floor(turns)), 0.65f, 1.0f, out[0], out[1], out[2]);
            return;
        }
        default: std::memcpy(out, s.vizColor, sizeof(float) * 3); return;
    }
}

// One layer: lines grouped in color buckets.
struct Layer {
    std::vector<sdk::draw3d::WorldShapes> buckets;
    const music::Settings* s = nullptr;
    double now = 0.0;
    float alpha = 1.0f;
    float lighten = 0.0f;  // 0..1: mix toward white (glow layers, hits)

    void Init(const music::Settings& settings, double t, float opacity, float light) {
        s = &settings;
        now = t;
        alpha = std::clamp(opacity, 0.0f, 1.0f);
        lighten = std::clamp(light, 0.0f, 1.0f);
        buckets.assign(kBuckets, sdk::draw3d::WorldShapes{});
        for (auto& b : buckets) b.lines = true;
    }
    void Line(float t, float level, const float a[3], const float b[3]) {
        const float key = s->vizColorMode == 2 ? level : t;
        const int i = std::clamp(static_cast<int>(key * (kBuckets - 1) + 0.5f), 0, kBuckets - 1);
        auto& shape = buckets[static_cast<std::size_t>(i)];
        if (shape.triangles.empty()) {
            float c[3];
            const float bucket = static_cast<float>(i) / (kBuckets - 1);
            Color(*s, s->vizColorMode == 2 ? 0.0f : bucket, s->vizColorMode == 2 ? bucket : level, now, c);
            for (float& v : c) v = v + (1.0f - v) * lighten;
            shape.color = sdk::draw3d::Color{c[0], c[1], c[2], alpha};
        }
        shape.triangles.insert(shape.triangles.end(), a, a + 3);
        shape.triangles.insert(shape.triangles.end(), b, b + 3);
    }
    void TakeInto(std::vector<sdk::draw3d::WorldShapes>& out) {
        for (auto& b : buckets) {
            if (!b.triangles.empty()) out.push_back(std::move(b));
        }
    }
};

// A layer and its reflection below the base line ("y0"), dimmer.
struct Painter {
    Layer main, mirror;
    bool reflect = false;
    float y0 = 0.0f;
    void Init(const music::Settings& s, double now, float opacity, float lighten, bool reflection, float base) {
        main.Init(s, now, opacity, lighten);
        mirror.Init(s, now, opacity * 0.4f, lighten);
        reflect = reflection;
        y0 = base;
    }
    void Line(float t, float level, const float a[3], const float b[3]) {
        main.Line(t, level, a, b);
        if (!reflect) return;
        const float ra[3] = {a[0], 2.0f * y0 - a[1], a[2]};
        const float rb[3] = {b[0], 2.0f * y0 - b[1], b[2]};
        mirror.Line(t, level, ra, rb);
    }
    void TakeInto(std::vector<sdk::draw3d::WorldShapes>& out) {
        main.TakeInto(out);
        mirror.TakeInto(out);
    }
};

struct Camera {
    float pos[3];
    float forward[3];
    float right[3];
    float scaleX, scaleY;
};

}  // namespace

void Draw(ImVec2 display) {
    const music::Settings& s = music::Get();
    audioviz::Want(true);
    audioviz::SetTuning(s.vizSensitivity, s.vizSmoothing);
    sdk::camera::View view;
    if (!sdk::draw3d::Available() || !sdk::camera::GetView(view)) {
        Stop();
        audioviz::Want(true);
        return;
    }
    const audioviz::Levels lv = audioviz::Get();
    const double now = rainbow::Seconds();
    const media::Track track = media::Current();
    const bool playing = track.playing;
    g_paletteCount = std::clamp(track.paletteCount, 1, 5);
    std::memcpy(g_palette, track.palette, sizeof(g_palette));
    Camera cam{};
    std::memcpy(cam.pos, view.pos, sizeof(cam.pos));
    {
        const float yaw = view.yaw * kPi / 180.0f, pitch = view.pitch * kPi / 180.0f;
        cam.forward[0] = -std::sin(yaw) * std::cos(pitch);
        cam.forward[1] = -std::sin(pitch);
        cam.forward[2] = std::cos(yaw) * std::cos(pitch);
        cam.right[0] = -std::cos(yaw);
        cam.right[1] = 0.0f;
        cam.right[2] = -std::sin(yaw);
        cam.scaleX = view.scaleX;
        cam.scaleY = view.scaleY;
    }

    // The base line: at a fixed height of the world (like a real horizon),
    // or following your eyes.
    const float y0 = s.vizFollowHeight ? view.pos[1] + s.vizHeight : s.vizWorldY;
    const float hit = playing ? lv.kick * s.vizHitReaction : 0.0f;       // 0..~2
    const float scaleH = s.vizScale * (playing ? 1.0f : 0.0f) * (1.0f + 0.18f * std::min(hit, 1.5f));
    const bool layers = s.vizLayers;
    Painter glow, body, caps;
    glow.Init(s, now, s.vizOpacity * 0.28f, 0.45f, s.vizReflection, y0);
    body.Init(s, now, s.vizOpacity, std::min(0.4f, hit * 0.2f), s.vizReflection, y0);
    caps.Init(s, now, std::min(1.0f, s.vizOpacity * 1.2f), 0.55f, s.vizReflection, y0);

    const float R = s.vizDistance;
    const float cx = view.pos[0], cz = view.pos[2];
    const float spin = static_cast<float>(std::fmod(now * s.vizSpin, 360.0)) * kPi / 180.0f;
    auto at = [&](float angle, float radius, float y, float* p) {
        p[0] = cx + std::sin(angle + spin) * radius;
        p[1] = y;
        p[2] = cz + std::cos(angle + spin) * radius;
    };
    // Is a place of the ring on screen (with a margin)? Off-screen bars are skipped.
    auto visible = [&](float angle, float radius, float y) {
        float p[3];
        at(angle, radius, y, p);
        const float d[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
        const float depth = d[0] * cam.forward[0] + d[1] * cam.forward[1] + d[2] * cam.forward[2];
        if (depth < 1.0f) return false;
        const float x = (d[0] * cam.right[0] + d[2] * cam.right[2]) * cam.scaleX / depth;
        return std::fabs(x) < 1.35f;
    };
    // How many lines make a bar of this world width look solid HERE: the
    // closer to the sides of the screen, the more the perspective stretches
    // it, so the more lines it needs (distance / depth^2).
    auto linesFor = [&](float worldWidth, float angle, float radius, float y) {
        float p[3];
        at(angle, radius, y, p);
        const float d[3] = {p[0] - cam.pos[0], p[1] - cam.pos[1], p[2] - cam.pos[2]};
        const float dist = std::max(1.0f, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        const float depth = std::max(1.0f, d[0] * cam.forward[0] + d[1] * cam.forward[1] + d[2] * cam.forward[2]);
        const float px = worldWidth * cam.scaleX * display.x * 0.5f * dist / (depth * depth);
        return std::clamp(static_cast<int>(std::ceil(px * 1.5f)), 2, 48);
    };
    // A filled vertical bar from "bottom" to "top", "width" radians wide.
    auto bar = [&](Painter& p, float t, float level, float center, float width, float radius, float bottom, float top) {
        const int lines = linesFor(width * radius, center, radius, (bottom + top) * 0.5f);
        for (int k = 0; k < lines; ++k) {
            const float off = (static_cast<float>(k) / static_cast<float>(lines - 1) - 0.5f) * width;
            float a[3], b[3];
            at(center + off, radius, bottom, a);
            at(center + off, radius, top, b);
            p.Line(t, level, a, b);
        }
    };
    // A thin horizontal cap (the peaks).
    auto cap = [&](Painter& p, float t, float level, float center, float width, float radius, float y) {
        for (int k = 0; k < 2; ++k) {
            float a[3], b[3];
            at(center - width * 0.5f, radius, y + k * 0.3f, a);
            at(center + width * 0.5f, radius, y + k * 0.3f, b);
            p.Line(t, level, a, b);
        }
    };

    const int style = std::clamp(s.vizStyle, 0, 6);
    if (style == kBars || style == kMirror || style == kPillars || style == kAurora) {
        const int n = style == kPillars ? std::max(8, s.vizBars / 6) : s.vizBars;
        const float slot = 2.0f * kPi / static_cast<float>(n);
        const float thick = std::clamp(s.vizThickness * (style == kPillars ? 1.2f : 1.0f), 0.1f, 1.0f);
        const float radius = style == kAurora ? R * 0.9f : R;
        for (int i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            float center = t * 2.0f * kPi;
            if (style == kAurora) center += std::sin(static_cast<float>(now) * 0.7f + t * 18.0f) * slot * 0.8f;
            const float fast = layers ? Around(lv.fast, i, n) : Around(lv.bands, i, n);
            const float smooth = Around(lv.bands, i, n);
            const float peak = Around(lv.peaks, i, n);
            const float h = std::max(0.25f, fast * scaleH);
            float bottom = y0, top = y0 + h;
            if (style == kMirror) {
                bottom = y0 - h * 0.5f;
                top = y0 + h * 0.5f;
            } else if (style == kAurora) {
                top = y0 + s.vizScale * 1.6f + 12.0f;
                bottom = top - (3.0f + fast * scaleH * 0.9f);
            }
            if (!visible(center, radius, (bottom + top) * 0.5f)) continue;
            // Glow behind: wider, with the slow levels.
            if (layers) {
                const float gh = std::max(0.25f, smooth * scaleH * 1.15f);
                float gb = y0, gt = y0 + gh;
                if (style == kMirror) {
                    gb = y0 - gh * 0.5f;
                    gt = y0 + gh * 0.5f;
                } else if (style == kAurora) {
                    gt = top + 2.0f;
                    gb = gt - (4.0f + smooth * scaleH);
                }
                bar(glow, t, smooth, center, slot * std::min(1.0f, thick * 1.7f), radius * 1.01f, gb, gt);
            }
            bar(body, t, fast, center, slot * thick, radius, bottom, top);
            if (layers && style != kAurora) {
                const float py = style == kMirror ? y0 + std::max(0.25f, peak * scaleH) * 0.5f : y0 + std::max(0.25f, peak * scaleH);
                cap(caps, t, peak, center, slot * thick, radius * 0.995f, py + 0.4f);
            }
        }
    } else if (style == kWave || style == kLayeredWave) {
        const int n = std::max(96, s.vizBars * 2);
        // Wave: the spectrum around the horizon. Layered wave: the SOUND
        // WAVE itself (spiky, like a music app's player), in 3 layers of
        // colors and sizes, each moving a little differently.
        const int layerCount = style == kLayeredWave ? 3 : (layers ? 2 : 1);
        for (int layer = 0; layer < layerCount; ++layer) {
            Painter& p = layer == 0 ? body : (layer == 1 ? glow : caps);
            const float amp = style == kLayeredWave ? scaleH * (0.55f - 0.15f * layer) * (0.4f + lv.level * 1.2f + hit * 0.4f) : scaleH;
            const int shift = layer * 37;
            const int thickness = layer == 0 ? 3 : 2;
            for (int i = 0; i < n; ++i) {
                const float t0 = static_cast<float>(i) / static_cast<float>(n);
                const float t1 = static_cast<float>(i + 1) / static_cast<float>(n);
                float v0, v1;
                if (style == kLayeredWave) {
                    // Around the ring, the wave goes there and back (mirrored) so it closes.
                    auto waveAt = [&](int k) {
                        const float u = static_cast<float>(k % n) / static_cast<float>(n);
                        const float fold = 1.0f - std::fabs(u * 2.0f - 1.0f);
                        const int idx = (static_cast<int>(fold * (audioviz::kWavePoints - 1)) + shift) % audioviz::kWavePoints;
                        return lv.wave[idx];
                    };
                    v0 = y0 + scaleH * 0.6f + waveAt(i) * amp;
                    v1 = y0 + scaleH * 0.6f + waveAt(i + 1) * amp;
                } else {
                    const float* src = layer == 0 ? lv.fast : lv.bands;
                    v0 = y0 + Around(src, i, n) * scaleH * (layer == 0 ? 1.0f : 1.15f);
                    v1 = y0 + Around(src, (i + 1) % n, n) * scaleH * (layer == 0 ? 1.0f : 1.15f);
                }
                if (!visible(t0 * 2.0f * kPi, R, (v0 + v1) * 0.5f)) continue;
                const float level = std::clamp((v0 - y0) / std::max(1.0f, scaleH), 0.0f, 1.0f);
                for (int k = 0; k < thickness; ++k) {
                    const float dy = (static_cast<float>(k) - (thickness - 1) * 0.5f) * 0.3f;
                    float a[3], b[3];
                    at(t0 * 2.0f * kPi, R, v0 + dy, a);
                    at(t1 * 2.0f * kPi, R, v1 + dy, b);
                    p.Line(t0, level, a, b);
                }
            }
        }
    } else if (style == kRings) {
        // Rings above you, one per group of pitches, growing with their sound;
        // a hit sends an extra ring outward.
        constexpr int kRingCount = 10;
        constexpr int kSegments = 96;
        const float y = y0 + std::max(20.0f, s.vizScale * 1.5f);
        auto ring = [&](Painter& p, float radius, float yy, float t, float level) {
            for (int i = 0; i < kSegments; ++i) {
                float a[3], b[3];
                at(static_cast<float>(i) / kSegments * 2.0f * kPi, radius, yy, a);
                at(static_cast<float>(i + 1) / kSegments * 2.0f * kPi, radius, yy, b);
                p.Line(t, level, a, b);
            }
        };
        for (int r = 0; r < kRingCount; ++r) {
            float fast = 0.0f, smooth = 0.0f;
            const int b0 = r * audioviz::kBands / kRingCount, b1 = (r + 1) * audioviz::kBands / kRingCount;
            for (int b = b0; b < b1; ++b) {
                fast = std::max(fast, lv.fast[b]);
                smooth = std::max(smooth, lv.bands[b]);
            }
            const float t = static_cast<float>(r) / (kRingCount - 1);
            const float base = R * (0.12f + 0.075f * static_cast<float>(r));
            const float grow = playing ? 1.0f : 0.0f;
            for (int k = 0; k < 2; ++k) ring(body, base * (1.0f + fast * 0.35f * grow), y + k * 0.4f, t, fast);
            if (layers) ring(glow, base * (1.0f + smooth * 0.5f * grow), y - 0.6f, t, smooth);
        }
        if (hit > 0.05f) ring(caps, R * (0.9f + (1.0f - std::min(1.0f, hit)) * 0.3f), y + 1.0f, 1.0f, 1.0f);
    }
    std::vector<sdk::draw3d::WorldShapes> shapes;
    glow.TakeInto(shapes);
    body.TakeInto(shapes);
    caps.TakeInto(shapes);
    sdk::draw3d::SetWorldShapes(std::move(shapes), sdk::draw3d::kChannelVisualizer);
    g_sent = true;
}

void Stop() {
    audioviz::Want(false);
    if (g_sent) {
        sdk::draw3d::SetWorldShapes({}, sdk::draw3d::kChannelVisualizer);
        g_sent = false;
    }
}

}  // namespace luma::render::visualizer
