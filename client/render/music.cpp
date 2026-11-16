// =============================================================================
//  music.cpp - How the music looks (see music.h)
// =============================================================================
#include "render/music.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <imgui_internal.h>  // ImLerp for colors

#include "core/audio_viz.h"
#include "core/features.h"
#include "core/spotify.h"
#include "core/tr.h"
#include "render/gpu.h"
#include "render/font_fallback.h"
#include "render/icons.h"
#include "render/lyric_flow.h"
#include "render/visualizer.h"
#include "render/world_draw.h"
#include "sdk/mouse_grab.h"
#include "render/rainbow.h"
#include "render/theme.h"
#include "render/ui.h"
#include "render/menu.h"
#include "render/overlay.h"

namespace luma::render::music {

namespace {

constexpr float kPi = 3.14159265f;

// Icons (24x24 SVG paths; "F" at the start = filled).
constexpr const char* kPlayIcon = icons::kPlayFilled;
constexpr const char* kPauseIcon = icons::kPauseFilled;
constexpr const char* kNextIcon = icons::kSkipForwardFilled;
constexpr const char* kPrevIcon = icons::kSkipBackFilled;
constexpr const char* kNoteIcon = icons::kMusic;

ImU32 U(ImVec4 c, float alpha) {
    c.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

float Ease(ImGuiID id, float target, float speed) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float v = storage->GetFloat(id, target);
    v = ImLerp(v, target, std::min(1.0f, ImGui::GetIO().DeltaTime * speed));
    storage->SetFloat(id, v);
    return v;
}

// A text cut with "..." so it fits in "maxWidth".
void Fit(ImFont* font, float size, const char* text, float maxWidth, char* out, std::size_t outSize) {
    strncpy_s(out, outSize, text, _TRUNCATE);
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, out).x <= maxWidth) {
        return;
    }
    std::size_t len = std::strlen(out);
    while (len > 1) {
        --len;
        while (len > 0 && (static_cast<unsigned char>(out[len]) & 0xC0) == 0x80) {
            --len;  // don't cut a UTF-8 letter in half
        }
        out[len] = '\0';
        char test[256];
        std::snprintf(test, sizeof(test), "%s...", out);
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, test).x <= maxWidth) {
            strncpy_s(out, outSize, test, _TRUNCATE);
            return;
        }
    }
}

// A round button on the Halo. Returns true when clicked.
bool HaloButton(ImDrawList* draw, ImGuiID id, ImVec2 at, float radius, const char* icon, bool primary, bool enabled,
                ImVec4 color, bool& over) {
    const theme::Palette& p = theme::P();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float dx = mouse.x - at.x, dy = mouse.y - at.y;
    const bool hovered = enabled && dx * dx + dy * dy <= radius * radius * 1.2f;
    const float h = Ease(id, hovered ? 1.0f : 0.0f, 16.0f);
    const float r = radius * (1.0f + 0.08f * h);
    if (hovered) {
        over = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const float alpha = enabled ? 1.0f : 0.35f;
    if (primary) {
        draw->AddCircleFilled(at, r, U(color, alpha), 40);
        icons::Draw(draw, icon, at, r * 1.05f, U(theme::kCharcoal, alpha));
    } else {
        draw->AddCircleFilled(at, r, U(p.surface, 0.92f * alpha), 40);
        draw->AddCircle(at, r, U(ImLerp(p.border, color, h * 0.7f), alpha), 40, 1.0f);
        icons::Draw(draw, icon, at, r * 0.95f, U(ImLerp(p.text, color, h), alpha));
    }
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

}  // namespace

Settings& Get() {
    static Settings settings;
    return settings;
}

ImTextureID ArtTexture() {
    static ImTextureID texture = 0;
    static std::uint32_t version = 0;
    static std::vector<std::uint8_t> pixels;
    std::uint32_t newVersion = 0;
    if (media::TakeArt(version, pixels, newVersion)) {
        if (texture == 0) {
            texture = gpu::CreateTexture(pixels.data(), media::kArtSize, media::kArtSize);
        } else {
            gpu::UpdateTexture(texture, pixels.data(), media::kArtSize, media::kArtSize);
        }
        version = newVersion;
    }
    const media::Track track = media::Current();
    return track.artVersion != 0 && track.artVersion == version ? texture : 0;
}

void FormatTime(double seconds, char* out, std::size_t size) {
    const int total = static_cast<int>(std::max(0.0, seconds));
    if (total >= 3600) {
        std::snprintf(out, size, "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::snprintf(out, size, "%d:%02d", total / 60, total % 60);
    }
}

ImVec4 AccentFor(int mode, const media::Track& track, ImVec4 fallback) {
    if (mode == 2) {
        float r, g, b;
        rainbow::Color(rainbow::kDefaultSpeed, 0.0f, r, g, b);
        return ImVec4(r, g, b, 1.0f);
    }
    if (mode == 1 && track.artVersion != 0) {
        // The album's color, but always bright enough to see on dark glass.
        float r = track.accent[0], g = track.accent[1], b = track.accent[2];
        const float mx = std::max({r, g, b, 0.001f});
        if (mx < 0.75f) {
            const float k = 0.75f / mx;
            r = std::min(1.0f, r * k);
            g = std::min(1.0f, g * k);
            b = std::min(1.0f, b * k);
        }
        return ImVec4(r, g, b, 1.0f);
    }
    return fallback;
}

namespace {

struct Tint {
    int frame = -1;
    bool has = false;
    float rgb[3]{};
};

// The album color, bright enough to read on the HUD and the menu. Once per frame.
const Tint& AlbumTint() {
    static Tint tint;
    const int frame = ImGui::GetFrameCount();
    if (tint.frame != frame) {
        tint.frame = frame;
        tint.has = false;
        const Settings& s = Get();
        if (s.active) {
            const media::Track t = media::Current();
            if (t.present && t.artVersion != 0) {
                const ImVec4 c = AccentFor(1, t, ImVec4(1, 1, 1, 1));
                tint.rgb[0] = c.x;
                tint.rgb[1] = c.y;
                tint.rgb[2] = c.z;
                tint.has = true;
            }
        }
    }
    return tint;
}

}  // namespace

bool LyricAnimations() {
    const Settings& s = Get();
    // Only the MUSIC efficiency mode decides this (the general one doesn't
    // touch the music: they're separate on purpose).
    return !(s.efficiency && s.effLyricAnim);
}
bool UiAnimations() {
    const Settings& s = Get();
    return !(s.efficiency && s.effUiAnim);
}
bool SpinOn() {
    const Settings& s = Get();
    return !(s.efficiency && s.effSpin);
}
bool SongFade() {
    const Settings& s = Get();
    return !(s.efficiency && s.effSongFade);
}
bool MarqueeOn() {
    const Settings& s = Get();
    return !(s.efficiency && s.effMarquee);
}
bool AlbumColorsOn() {
    const Settings& s = Get();
    return !(s.efficiency && s.effAlbumColors);
}
bool HdCoversOn() {
    const Settings& s = Get();
    return s.hdCovers && !(s.efficiency && s.effHdCovers);
}

void ApplyEfficiency() {
    const Settings& s = Get();
    lyrics::SetHdCovers(HdCoversOn());
    lyrics::Want(lyrics::kCovers, HdCoversOn() && s.active);
    spotify::SetPrefetch(!(s.efficiency && s.effPrefetch));
}

bool DrawEfficiencySettings() {
    Settings& s = Get();
    bool changed = false;
    ImGui::PushID("musicEfficiency");
    struct PopId {
        ~PopId() { ImGui::PopID(); }
    } popId;
    ui::Section("Efficiency");
    changed |= ui::Toggle("Music efficiency mode", &s.efficiency, "(lighter music module)");
    ui::Hint("Turns off the music's extras: lyric animations, the spinning art, fades, scrolling names, album "
             "colors and the background downloads (HD art, lyrics ahead). The music and its lyrics keep working.");
    static bool advanced = false;
    ui::Toggle("Advanced: choose what it turns off", &advanced);
    if (advanced) {
        ImGui::BeginDisabled(!s.efficiency);
        changed |= ui::Toggle("Lyric animations", &s.effLyricAnim, "(sweep, rising words, fades)");
        changed |= ui::Toggle("Music page animations", &s.effUiAnim, "(buttons and controls)");
        changed |= ui::Toggle("Spinning art", &s.effSpin);
        changed |= ui::Toggle("Song change fade", &s.effSongFade);
        changed |= ui::Toggle("Scrolling names", &s.effMarquee);
        changed |= ui::Toggle("Album colors", &s.effAlbumColors, "(HUD, menu, crosshair, watermark)");
        changed |= ui::Toggle("HD album art", &s.effHdCovers, "(no downloads)");
        changed |= ui::Toggle("Lyrics ahead (Spotify)", &s.effPrefetch, "(no lookups for the next songs)");
        changed |= ui::Toggle("Visualizer", &s.effVisualizer, "(it listens to the sound and draws a lot)");
        ImGui::EndDisabled();
    }
    if (changed) ApplyEfficiency();
    return changed;
}

bool HudTint(float out[3]) {
    if (!AlbumColorsOn()) return false;
    const Tint& t = AlbumTint();
    if (!t.has || !Get().hudFollowAlbum) return false;
    std::memcpy(out, t.rgb, sizeof(t.rgb));
    return true;
}

bool AlbumColor(float out[3]) {
    if (!AlbumColorsOn()) return false;
    const Tint& t = AlbumTint();
    if (!t.has) return false;
    std::memcpy(out, t.rgb, sizeof(t.rgb));
    return true;
}

float ProgressSpring::Update(float playing, std::uint32_t trackId, double duration, float follow) {
    const double now = ImGui::GetTime();
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    playing = std::clamp(playing, 0.0f, 1.0f);
    pop *= std::exp(-dt * 5.0f);
    if (trackId != track || x < 0.0f) {
        // A new song: go straight there, no animation.
        track = trackId;
        x = playing;
        hold = -1.0f;
        startedAt = -1.0;
        trip = 1.0f;
        v = 0.0f;
        pop = 0.0f;
        return x;
    }
    if (follow >= 0.0f) {
        // Dragging: the dot follows the mouse closely (a touch of smoothing).
        x += (std::clamp(follow, 0.0f, 1.0f) - x) * std::min(1.0f, dt * 28.0f);
        startedAt = -1.0;
        return x;
    }
    // Stop holding once the app reports (about) the place we seeked to.
    if (hold >= 0.0f && startedAt < 0.0 && (std::fabs(playing - hold) * duration < 1.2 || now > holdUntil)) hold = -1.0f;
    if (startedAt >= 0.0) {
        // The spring (damping ratio ~0.6: a little past, then back) on the
        // trip's progress, in small steps so it's the same at any frame rate.
        constexpr float k = 130.0f, c = 14.0f;
        for (int i = 0; i < 4; ++i) {
            const float h = dt / 4.0f;
            v += (k * (1.0f - trip) - c * v) * h;
            trip += v * h;
        }
        const float delta = hold - from;
        const float inside = std::min(trip, 1.0f);
        float past = (trip - 1.0f) * delta;          // how far past the target...
        past = std::clamp(past, -0.02f, 0.02f);      // ...at most 2% of the bar
        x = from + delta * inside + (trip > 1.0f ? past : 0.0f);
        if (std::fabs(1.0f - trip) < 0.002f && std::fabs(v) < 0.02f) {
            x = hold;
            startedAt = -1.0;
        }
        return std::clamp(x, 0.0f, 1.0f);
    }
    x = hold >= 0.0f ? hold : playing;
    return x;
}

void ProgressSpring::SeekTo(float frac, double duration) {
    from = x < 0.0f ? frac : x;
    hold = std::clamp(frac, 0.0f, 1.0f);
    holdUntil = ImGui::GetTime() + 2.5;
    startedAt = ImGui::GetTime();
    trip = 0.0f;
    v = 0.0f;
    pop = 1.0f;
    media::Seek(hold * duration);
}

bool MenuTint(float out[3]) {
    if (!AlbumColorsOn()) return false;
    const Tint& t = AlbumTint();
    if (!t.has || !Get().menuFollowAlbum) return false;
    std::memcpy(out, t.rgb, sizeof(t.rgb));
    return true;
}

float SpinAngle() {
    static float angle = 0.0f;
    static int lastFrame = -1;
    const int frame = ImGui::GetFrameCount();
    if (frame != lastFrame) {
        lastFrame = frame;
        if (media::Current().playing && SpinOn()) {
            angle = std::fmod(angle + ImGui::GetIO().DeltaTime * 0.9f, 2.0f * kPi);
        }
    }
    return angle;
}

void DrawArt(ImDrawList* draw, ImVec2 min, ImVec2 max, int shape, float angle, float alpha) {
    const ImTextureID texture = ArtTexture();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float half = std::min(max.x - min.x, max.y - min.y) * 0.5f;
    const float rounding = shape == 1 ? half * 0.22f : 0.0f;
    const ImU32 white = U(ImVec4(1, 1, 1, 1), alpha);
    if (texture == 0) {
        const ImU32 dark = U(ImVec4(0.23f, 0.24f, 0.25f, 1.0f), alpha);
        if (shape == 2) {
            draw->AddCircleFilled(center, half, dark, 48);
        } else {
            draw->AddRectFilled(min, max, dark, rounding);
        }
        icons::Draw(draw, kNoteIcon, center, half * 1.0f, U(ImVec4(0.91f, 0.90f, 0.87f, 0.55f), alpha));
        return;
    }
    if (shape != 2) {
        draw->AddImageRounded(texture, min, max, ImVec2(0, 0), ImVec2(1, 1), white, rounding);
        return;
    }
    // Round art: a fan of triangles whose texture coordinates are turned by
    // "angle", so the picture spins like a record.
    constexpr int kSegments = 56;
    draw->PushTextureID(texture);
    draw->PrimReserve(kSegments * 3, kSegments + 1);
    const ImDrawIdx base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    draw->PrimWriteVtx(center, ImVec2(0.5f, 0.5f), white);
    for (int i = 0; i < kSegments; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / kSegments;
        const ImVec2 pos(center.x + std::cos(a) * half, center.y + std::sin(a) * half);
        const ImVec2 uv(0.5f + std::cos(a - angle) * 0.5f, 0.5f + std::sin(a - angle) * 0.5f);
        draw->PrimWriteVtx(pos, uv, white);
    }
    for (int i = 0; i < kSegments; ++i) {
        draw->PrimWriteIdx(base);
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
        draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % kSegments));
    }
    draw->PopTextureID();
    // A thin ring and a subFont hole, so it reads as a disc.
    draw->AddCircle(center, half, U(ImVec4(0, 0, 0, 0.35f), alpha), kSegments, 1.0f);
    draw->AddCircleFilled(center, half * 0.07f, U(ImVec4(0.09f, 0.09f, 0.10f, 0.9f), alpha), 20);
}

ProgressSpring g_haloSpring;

bool DrawHalo(ImDrawList* draw, ImVec2 c, float outer, float inner, float appear, bool& drewCenter) {
    drewCenter = false;
    const Settings& s = Get();
    if (!s.active || !s.halo) {
        return false;
    }
    const media::Track t = media::Current();
    if (!t.present) {
        return false;
    }
    const theme::Palette& p = theme::P();
    const float fs = ImGui::GetFontSize();
    const ImVec4 color = AccentFor(s.haloColor, t, theme::Accent(1.0f));
    const float th = std::max(1.5f, s.haloThickness * fs);
    const float r = outer + s.haloGap * fs;
    const float top = -kPi * 0.5f;  // the progress starts at the top and goes clockwise
    bool over = false;

    // Where the buttons go (decided first: a click on a button must never
    // also start dragging the ring underneath it).
    const bool buttons = s.haloButtons != 2;
    const float rb = fs * 0.95f, rp = fs * 1.2f;
    float aPrev = 0.0f, aPlay = 0.0f, aNext = 0.0f;  // radians (0 = right, down = +90 degrees)
    if (s.haloButtons == 0) {                       // close together under the ring
        aPrev = (90.0f + 16.0f) * kPi / 180.0f;
        aPlay = 90.0f * kPi / 180.0f;
        aNext = (90.0f - 16.0f) * kPi / 180.0f;
    } else {                                        // around it: left, bottom, right
        aPrev = kPi;
        aPlay = kPi * 0.5f;
        aNext = 0.0f;
    }
    auto at = [&](float a) { return ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r); };
    bool overButton = false;
    if (buttons) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const ImVec2 spots[3] = {at(aPrev), at(aPlay), at(aNext)};
        const float radii[3] = {rb, rp, rb};
        for (int i = 0; i < 3; ++i) {
            const float bx = mp.x - spots[i].x, by = mp.y - spots[i].y;
            overButton |= bx * bx + by * by <= radii[i] * radii[i] * 1.3f;
        }
    }

    // 1. The full ring (dim) and the progress.
    if (s.haloTrack) {
        draw->AddCircle(c, r, U(color, 0.16f), 128, th);
    }
    const float frac = t.duration > 0.0 ? static_cast<float>(std::clamp(t.position / t.duration, 0.0, 1.0)) : 0.0f;
    static bool dragging = false;
    static float dragFrac = 0.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float mdx = mouse.x - c.x, mdy = mouse.y - c.y;
    const float mdist = std::sqrt(mdx * mdx + mdy * mdy);
    float mangle = std::atan2(mdy, mdx) - top;
    if (mangle < 0.0f) mangle += 2.0f * kPi;
    const float mouseFrac = mangle / (2.0f * kPi);
    static bool pressed = false;   // the mouse went down on the ring...
    static ImVec2 pressAt;         // ...here
    if (t.canSeek && t.duration > 0.0 && s.haloKnob) {
        const bool nearRing = !overButton && std::fabs(mdist - r) < th * 2.0f + fs * 0.35f;
        if (nearRing) {
            over = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                pressed = true;
                pressAt = mouse;
            }
        }
        if (pressed) {
            over = true;
            // It's a drag only once the mouse moves a little; a plain click stays a click.
            const float mx = mouse.x - pressAt.x, my = mouse.y - pressAt.y;
            if (!dragging && mx * mx + my * my > fs * fs * 0.25f) dragging = true;
            if (dragging) dragFrac = mouseFrac;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // Release: travel there with the spring (from where the dot is now).
                g_haloSpring.SeekTo(dragging ? dragFrac : mouseFrac, t.duration);
                dragging = false;
                pressed = false;
            }
        }
    } else {
        dragging = false;
        pressed = false;
    }
    const float shown = g_haloSpring.Update(frac, t.trackId, t.duration, dragging ? dragFrac : -1.0f);
    if (shown > 0.002f) {
        const float a1 = top + shown * 2.0f * kPi;
        draw->PathArcTo(c, r, top, a1, std::max(6, static_cast<int>(128 * shown)));
        draw->PathStroke(U(color, 0.95f), ImDrawFlags_None, th);
        // round ends
        draw->AddCircleFilled(ImVec2(c.x + std::cos(top) * r, c.y + std::sin(top) * r), th * 0.5f, U(color, 0.95f), 16);
        draw->AddCircleFilled(ImVec2(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r), th * 0.5f, U(color, 0.95f), 16);
    }
    if (s.haloKnob && t.duration > 0.0) {
        const float a1 = top + shown * 2.0f * kPi;
        const float grow = Ease(ImGui::GetID("##haloKnob"), over || dragging ? 1.0f : 0.0f, 14.0f);
        const float beat = g_haloSpring.pop;  // right after a seek the dot beats once
        const ImVec2 knob(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r);
        if (beat > 0.02f) {
            draw->AddCircle(knob, th * (1.8f + 3.0f * (1.0f - beat)), U(color, 0.6f * beat), 24, std::max(1.0f, th * 0.4f));
        }
        draw->AddCircleFilled(knob, th * (1.35f + 0.6f * grow + 0.5f * beat), U(color, 1.0f), 24);
    }

    // 2. Title and artist above the ring (and the time while dragging).
    if (s.haloTitle && t.title[0] != '\0') {
        ImFont* titleFont = theme::GetFonts().medium;
        ImFont* subFont = theme::GetFonts().regular;
        const float maxW = r * 1.7f;
        char title[200], artist[200];
        Fit(titleFont, titleFont->FontSize, t.title, maxW, title, sizeof(title));
        Fit(subFont, subFont->FontSize, t.artist, maxW, artist, sizeof(artist));
        const float yArtist = c.y - r - th - fs * 1.35f;
        const float yTitle = yArtist - titleFont->FontSize * 1.05f;
        const ImVec2 ts = titleFont->CalcTextSizeA(titleFont->FontSize, FLT_MAX, 0.0f, title);
        draw->AddText(titleFont, titleFont->FontSize, ImVec2(c.x - ts.x * 0.5f, yTitle), U(p.text, 1.0f), title);
        if (artist[0] != '\0') {
            const ImVec2 as = subFont->CalcTextSizeA(subFont->FontSize, FLT_MAX, 0.0f, artist);
            draw->AddText(subFont, subFont->FontSize, ImVec2(c.x - as.x * 0.5f, yArtist), U(p.textDim, 1.0f), artist);
        }
    }
    if ((dragging || (over && t.duration > 0.0)) && t.duration > 0.0) {
        char now[16], total[16], line[40];
        FormatTime((dragging ? dragFrac : shown) * t.duration, now, sizeof(now));
        FormatTime(t.duration, total, sizeof(total));
        std::snprintf(line, sizeof(line), "%s / %s", now, total);
        const ImVec2 ls = ImGui::CalcTextSize(line);
        draw->AddText(ImVec2(c.x - ls.x * 0.5f, c.y - inner - fs * 1.6f), U(p.text, 0.9f), line);
    }

    // 3. The buttons.
    if (buttons) {
        if (HaloButton(draw, ImGui::GetID("##haloPrev"), at(aPrev), rb, kPrevIcon, false, t.canPrevious, color, over)) {
            media::Previous();
        }
        if (HaloButton(draw, ImGui::GetID("##haloNext"), at(aNext), rb, kNextIcon, false, t.canNext, color, over)) {
            media::Next();
        }
        if (HaloButton(draw, ImGui::GetID("##haloPlay"), at(aPlay), rp, t.playing ? kPauseIcon : kPlayIcon, true,
                       t.canPlayPause, color, over)) {
            media::PlayPause();
        }
    }

    // 4. The album art in the middle, instead of your head.
    if (s.haloArtCenter && ArtTexture() != 0) {
        const float rr = inner * 0.9f;
        DrawArt(draw, ImVec2(c.x - rr, c.y - rr), ImVec2(c.x + rr, c.y + rr), s.haloArtShape,
                s.haloArtShape == 2 && s.spinArt ? SpinAngle() : 0.0f, appear);
        drewCenter = true;
    }
    return over;
}


bool DrawStyleSettings() {
    ui::Section("On screen");
    return DrawWholePage(kPageOnScreen);
}

bool DrawHaloSettings() {
    ui::Section("Halo (radial menu)");
    return DrawWholePage(kPageHalo);
}


// =============================================================================
//  Lyrics
// =============================================================================

const lyrics::Result& CurrentLyrics() {
    static lyrics::Result result;
    static std::uint32_t version = 0;
    lyrics::Result fresh;
    std::uint32_t v = 0;
    if (lyrics::Snapshot(version, fresh, v)) {
        result = std::move(fresh);
        version = v;
        // Lyrics in another alphabet: make sure the fonts have its letters.
        for (const lyrics::Line& line : result.lines) fontfallback::Need(line.text.c_str());
    }
    // ... and the song's name, artist and album.
    static std::uint32_t lastTrack = 0;
    const media::Track t = media::Current();
    if (t.trackId != lastTrack) {
        lastTrack = t.trackId;
        fontfallback::Need(t.title);
        fontfallback::Need(t.artist);
        fontfallback::Need(t.album);
    }
    return result;
}

int LyricsMs(const media::Track& track) {
    const int raw = static_cast<int>((track.position - static_cast<double>(Get().lyricsOffset)) * 1000.0);
    // A STEADY CLOCK: the app corrects the song's position now and then, and
    // the correction can step back a few milliseconds. A lit word would then
    // go out and on again (a "stutter"). While the song plays, small steps
    // back (under 0.4 s) are ignored and the clock catches up smoothly; a
    // bigger jump (a seek) or a new song is taken at once.
    static int shown = 0;
    static std::uint32_t lastTrack = 0;
    static int frame = -1;
    if (frame == ImGui::GetFrameCount() && lastTrack == track.trackId) return shown;  // once per frame
    frame = ImGui::GetFrameCount();
    if (track.trackId != lastTrack || !track.playing || raw > shown + 400 || raw < shown - 400) {
        shown = raw;
    } else if (raw > shown) {
        shown = raw;
    }
    lastTrack = track.trackId;
    return shown;
}

float WordProgress(const lyrics::Result& ly, int line, std::size_t word, int ms) {
    if (line < 0 || line >= static_cast<int>(ly.lines.size())) return 0.0f;
    const lyrics::Line& l = ly.lines[static_cast<std::size_t>(line)];
    if (word >= l.words.size()) return 0.0f;
    const int start = l.words[word].ms;
    int end;
    if (word + 1 < l.words.size()) {
        end = l.words[word + 1].ms;
    } else {
        const int nextLine = line + 1 < static_cast<int>(ly.lines.size()) ? ly.lines[static_cast<std::size_t>(line + 1)].ms
                                                                            : start + 1500;
        end = std::min(nextLine, start + 1500);
    }
    // At least 120 ms: a word that comes "instantly" still gets a quick sweep.
    const int duration = std::max(120, end - start);
    return std::clamp(static_cast<float>(ms - start) / static_cast<float>(duration), 0.0f, 1.0f);
}

void FeatherRight(ImDrawList* draw, int firstVertex, float edgeX, float feather) {
    feather = std::max(feather, 1.0f);
    for (int i = firstVertex; i < draw->VtxBuffer.Size; ++i) {
        ImDrawVert& v = draw->VtxBuffer[i];
        const float k = std::clamp((edgeX - v.pos.x) / feather + 0.5f, 0.0f, 1.0f);
        const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(a) * k + 0.5f) << IM_COL32_A_SHIFT);
    }
}

namespace {

// Splits "text" into rows that fit "width" (word wrap).
int WrapRows(ImFont* font, float size, const char* text, float width, const char** starts, const char** ends, int max) {
    int count = 0;
    const char* s = text;
    const char* end = text + std::strlen(text);
    while (s < end && count < max) {
        const char* cut = font->CalcWordWrapPositionA(size / font->FontSize, s, end, width);
        if (cut == s) cut = s + 1;  // a single huge word: at least one letter per row
        starts[count] = s;
        ends[count] = cut;
        ++count;
        s = cut;
        while (s < end && *s == ' ') ++s;
    }
    return count;
}

float RowsHeight(ImFont* font, float size, const char* text, float width) {
    const char* a[8];
    const char* b[8];
    const int rows = std::max(1, WrapRows(font, size, text, width, a, b, 8));
    return rows * size * 1.18f;
}

}  // namespace

void DrawLyrics(ImVec2 min, ImVec2 max) {
    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const Settings& s = Get();
    const media::Track t = media::Current();
    const lyrics::Result& ly = CurrentLyrics();
    ImFont* big = theme::GetFonts().title;
    ImFont* mid = theme::GetFonts().medium;
    ImFont* reg = theme::GetFonts().regular;
    const float fs = ImGui::GetFontSize();
    const float width = max.x - min.x;
    const ImVec4 accent = AccentFor(s.haloColor, t, theme::Accent(1.0f));
    draw->PushClipRect(min, max, true);

    auto centerMessage = [&](const char* line1, const char* line2) {
        const ImVec2 s1 = mid->CalcTextSizeA(mid->FontSize, FLT_MAX, 0.0f, line1);
        const float y = min.y + (max.y - min.y) * 0.35f;
        draw->AddText(mid, mid->FontSize, ImVec2(min.x + (width - s1.x) * 0.5f, y), U(p.text, 1.0f), line1);
        if (line2 != nullptr) {
            const ImVec2 s2 = reg->CalcTextSizeA(reg->FontSize, FLT_MAX, 0.0f, line2);
            draw->AddText(reg, reg->FontSize, ImVec2(min.x + (width - s2.x) * 0.5f, y + mid->FontSize * 1.4f),
                          U(p.textDim, 1.0f), line2);
        }
    };

    const bool forThisSong = t.present && ly.trackId == t.trackId;
    if (!t.present || t.title[0] == '\0') {
        centerMessage(Tr("Nothing is playing"), nullptr);
    } else if (!forThisSong || ly.status == lyrics::Status::Searching || ly.status == lyrics::Status::Idle) {
        centerMessage(Tr("Looking for the lyrics..."), nullptr);
    } else if (ly.status == lyrics::Status::Instrumental) {
        centerMessage(Tr("Instrumental"), Tr("This song has no lyrics."));
    } else if (ly.status == lyrics::Status::Offline) {
        centerMessage(Tr("Online lyrics are off"), Tr("Turn on \"Search lyrics online\" in Styles."));
    } else if (ly.status != lyrics::Status::Found) {
        centerMessage(Tr("No lyrics found for this song"), Tr("Sources keep growing: Luma will try again in a few days."));
    } else if (!ly.synced) {
        // Plain lyrics: all of them, scrolling with the mouse wheel.
        static float scroll = 0.0f;
        static std::uint32_t lastTrack = 0;
        if (lastTrack != t.trackId) {
            lastTrack = t.trackId;
            scroll = 0.0f;
        }
        if (ImGui::IsMouseHoveringRect(min, max)) scroll -= ImGui::GetIO().MouseWheel * fs * 3.0f;
        float total = 0.0f;
        for (const lyrics::Line& line : ly.lines) total += RowsHeight(mid, mid->FontSize, line.text.c_str(), width);
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, total - (max.y - min.y) + fs * 2.0f));
        float y = min.y - scroll;
        for (const lyrics::Line& line : ly.lines) {
            const float h = RowsHeight(mid, mid->FontSize, line.text.c_str(), width);
            if (y + h > min.y && y < max.y) {
                draw->AddText(mid, mid->FontSize, ImVec2(min.x, y), U(p.text, 0.85f), line.text.c_str(), nullptr, width);
            }
            y += h;
        }
    } else {
        // Synced lyrics (inspired by Liner): every line big and bold, the one
        // being sung bright and the rest soft; the list glides up by itself.
        // A pause in the song shows a note. Clicking a line jumps there.
        // Only REAL timings are used: word by word only if the source has
        // word times; otherwise the whole line lights up.
        ImFont* f = theme::GetFonts().lyrics != nullptr ? theme::GetFonts().lyrics : big;
        const float size = f->FontSize;
        const float rowH = size * 1.16f;
        const float gapH = size * 0.62f;
        const int ms = LyricsMs(t);
        const int current = lyrics::LineAt(ly, ms);
        const int n = static_cast<int>(ly.lines.size());
        // The list glides to the line being sung with a spring: it starts
        // gently, speeds up and settles without bouncing (critically damped).
        static float scroll = 0.0f;
        static float scrollSpeed = 0.0f;
        static std::uint32_t lastTrack = 0;
        const float target = static_cast<float>(std::max(0, current));
        if (lastTrack != t.trackId) {
            lastTrack = t.trackId;
            scroll = target;
            scrollSpeed = 0.0f;
        }
        {
            const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
            constexpr float kStiffness = 70.0f;
            const float damping = 2.0f * std::sqrt(kStiffness);
            scrollSpeed += (kStiffness * (target - scroll) - damping * scrollSpeed) * dt;
            scroll += scrollSpeed * dt;
            if (!LyricAnimations()) {  // efficiency: jump to the line
                scroll = target;
                scrollSpeed = 0.0f;
            }
            if (std::fabs(target - scroll) > 6.0f) {  // a big jump (seek): don't fly through the whole song
                scroll = target - std::copysign(6.0f, target - scroll);
            }
        }
        auto isPause = [&](int i) {
            const std::string& text = ly.lines[static_cast<std::size_t>(i)].text;
            return text.find_first_not_of(" \t") == std::string::npos;
        };
        auto heightOf = [&](int i) {
            if (isPause(i)) return size * 1.1f + gapH;
            return RowsHeight(f, size, ly.lines[static_cast<std::size_t>(i)].text.c_str(), width) * (rowH / (size * 1.18f)) + gapH;
        };
        const int base = std::clamp(static_cast<int>(std::floor(scroll)), 0, std::max(0, n - 1));
        const float frac = scroll - static_cast<float>(base);
        const float anchor = min.y + (max.y - min.y) * 0.36f;
        float y = anchor - frac * heightOf(base);
        int first = base;
        while (first > 0 && y > min.y) {
            --first;
            y -= heightOf(first);
        }
        // Before the first line (the intro): a note at the top.
        if (current < 0 && n > 0 && ly.lines[0].ms > 2500) {
            const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            icons::Draw(draw, kNoteIcon, ImVec2(min.x + size * 0.45f, anchor - size * 1.2f), size * 0.9f,
                        U(p.text, 0.5f + 0.5f * pulse), size * 0.09f);
        }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (int i = first; i < n && y < max.y; ++i) {
            const lyrics::Line& line = ly.lines[static_cast<std::size_t>(i)];
            const float h = heightOf(i);
            const float dist = std::fabs(static_cast<float>(i) - scroll);
            const float soft = std::clamp(0.34f - (dist - 1.0f) * 0.03f, 0.14f, 0.34f);
            const bool isCurrent = i == current;
            // Lines light up and go out gradually (by the song's clock, so a
            // pause freezes them too): 0.25 s in, 0.45 s out.
            const bool animate = LyricAnimations();
            const float lineOn = isCurrent ? (animate ? SmoothStep(static_cast<float>(ms - line.ms) / 250.0f) : 1.0f) : 0.0f;
            const float lineOff = (animate && i == current - 1 && i + 1 < n)
                                      ? 1.0f - SmoothStep(static_cast<float>(ms - ly.lines[static_cast<std::size_t>(i + 1)].ms) / 450.0f)
                                      : 0.0f;
            // Hover: a line can be clicked to jump to it.
            const bool hovered = t.canSeek && mouse.x >= min.x && mouse.x <= max.x && mouse.y >= y &&
                                 mouse.y < y + h - gapH * 0.5f && ImGui::IsWindowHovered();
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    media::Seek(static_cast<double>(line.ms) / 1000.0 + static_cast<double>(s.lyricsOffset));
                }
            }
            if (isPause(i)) {
                // A pause: a note (it breathes while it's the current "line").
                const float pulse = isCurrent ? 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f) : 0.0f;
                icons::Draw(draw, kNoteIcon, ImVec2(min.x + size * 0.45f, y + size * 0.5f), size * (0.85f + 0.1f * pulse),
                            U(p.text, isCurrent ? 0.6f + 0.4f * pulse : soft), size * 0.09f);
            } else if (isCurrent && ly.wordSynced && !line.words.empty()) {
                // Real word timings. Each word is drawn twice: dim, and a
                // bright copy on top whose soft edge sweeps left to right at
                // the speed the word is sung. The sung word also rises a hair.
                const float dimAlpha = soft + (0.4f - soft) * lineOn;
                const float feather = size * 0.55f;
                float x = min.x, rowY = y;
                for (std::size_t w = 0; w < line.words.size(); ++w) {
                    const lyrics::Word& word = line.words[w];
                    const ImVec2 ws = f->CalcTextSizeA(size, FLT_MAX, 0.0f, word.text.c_str());
                    // Wrap whole words (a word may come in syllables: "Elys"+"é"+"es ").
                    const bool wordStart = w == 0 || line.words[w - 1].text.back() == ' ';
                    float wordW = 0.0f;
                    if (wordStart) {
                        for (std::size_t k = w; k < line.words.size(); ++k) {
                            const std::string& t2 = line.words[k].text;
                            std::size_t len = t2.size();
                            while (len > 0 && t2[len - 1] == ' ') --len;
                            wordW += f->CalcTextSizeA(size, FLT_MAX, 0.0f, t2.c_str(), t2.c_str() + len).x;
                            if (len < t2.size()) break;
                        }
                    }
                    if (wordStart && x > min.x && x + wordW > max.x) {
                        x = min.x;
                        rowY += rowH;
                    }
                    const float sung = animate ? WordProgress(ly, i, w, ms) : (ms >= word.ms ? 1.0f : 0.0f);
                    const ImVec2 at(x, rowY - (animate ? size * 0.04f * SmoothStep(sung) : 0.0f));
                    draw->AddText(f, size, at, U(p.text, dimAlpha), word.text.c_str());
                    if (sung > 0.0f) {
                        const int firstVertex = draw->VtxBuffer.Size;
                        draw->AddText(f, size, at, U(p.text, 1.0f), word.text.c_str());
                        // From fully hidden (edge before the word) to fully lit (edge after it).
                        FeatherRight(draw, firstVertex, x - feather * 0.5f + sung * (ws.x + feather), feather);
                    }
                    x += ws.x;
                }
            } else {
                const char* starts[8];
                const char* ends[8];
                const int rows = WrapRows(f, size, line.text.c_str(), width, starts, ends, 8);
                float alpha = isCurrent ? soft + (1.0f - soft) * lineOn : soft;
                alpha = std::max(alpha, soft + (1.0f - soft) * lineOff);  // the line that just ended fades out
                if (hovered) alpha = std::max(alpha, soft + 0.25f);
                for (int r = 0; r < rows; ++r) {
                    draw->AddText(f, size, ImVec2(min.x, y + static_cast<float>(r) * rowH), U(p.text, alpha), starts[r], ends[r]);
                }
            }
            y += h;
        }
    }
    // The source, at the bottom.
    if (forThisSong && ly.status == lyrics::Status::Found) {
        char info[96];
        std::snprintf(info, sizeof(info), "%s  ·  %s", ly.wordSynced ? Tr("Word by word") : ly.synced ? Tr("Line by line") : Tr("Plain text"),
                      ly.source.c_str());
        const float h = reg->FontSize * 1.6f;
        draw->AddRectFilledMultiColor(ImVec2(min.x, max.y - h * 2.0f), ImVec2(max.x, max.y), U(p.surface, 0.0f),
                                      U(p.surface, 0.0f), U(p.surface, 0.9f), U(p.surface, 0.9f));
        draw->AddText(reg, reg->FontSize, ImVec2(min.x, max.y - reg->FontSize * 1.3f), U(p.textDim, 0.9f), info);
    }
    draw->PopClipRect();
}

bool DrawAlbumColorSettings() {
    ui::Section("Album colors");
    return DrawWholePage(kPageColors);
}

bool DrawLyricsSettings() {
    ui::Section("Lyric Flow");
    bool changed = DrawWholePage(kPageLyricFlow);
    ui::Section("Lyrics");
    changed |= DrawWholePage(kPageLyrics);
    return changed;
}

// =============================================================================
//  Spotify: "Up next" and its settings
// =============================================================================

namespace {

const std::vector<spotify::QueueItem>& CurrentQueue() {
    static std::vector<spotify::QueueItem> queue;
    static std::uint32_t version = 0;
    std::vector<spotify::QueueItem> fresh;
    std::uint32_t v = 0;
    if (spotify::Queue(version, fresh, v)) {
        queue = std::move(fresh);
        version = v;
        for (const auto& item : queue) {
            fontfallback::Need(item.title.c_str());
            fontfallback::Need(item.artist.c_str());
        }
    }
    return queue;
}

}  // namespace

void DrawUpNext(ImVec2 min, ImVec2 max) {
    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* mid = theme::GetFonts().medium;
    ImFont* reg = theme::GetFonts().regular;
    const float fs = ImGui::GetFontSize();
    const float width = max.x - min.x;
    const spotify::Status status = spotify::GetStatus();
    auto message = [&](const char* a, const char* b) {
        const float y = min.y + (max.y - min.y) * 0.3f;
        const ImVec2 sa = mid->CalcTextSizeA(mid->FontSize, FLT_MAX, 0.0f, a);
        draw->AddText(mid, mid->FontSize, ImVec2(min.x + (width - sa.x) * 0.5f, y), U(p.text, 1.0f), a);
        if (b != nullptr) {
            const ImVec2 sb = reg->CalcTextSizeA(reg->FontSize, FLT_MAX, width * 0.9f, b);
            draw->AddText(reg, reg->FontSize, ImVec2(min.x + (width - sb.x) * 0.5f, y + mid->FontSize * 1.5f), U(p.textDim, 1.0f), b,
                          nullptr, width * 0.9f);
        }
    };
    if (status.state != spotify::State::Connected) {
        message(Tr("Connect Spotify to see what's next"),
                Tr("Optional. In Styles > Spotify: paste your app's Client ID and press Connect. Luma then shows your "
                   "queue here and gets the lyrics of the next songs ready before they start."));
        return;
    }
    const auto& queue = CurrentQueue();
    if (queue.empty()) {
        message(Tr("Nothing in your queue"), Tr("Play something on Spotify (a playlist or an album)."));
        return;
    }
    // A list: number, title and artist, length on the right. The first one
    // (the next song) a bit brighter.
    float y = min.y + fs * 0.2f;
    const float rowH = mid->FontSize + reg->FontSize + fs * 0.9f;
    for (std::size_t i = 0; i < queue.size() && y + rowH <= max.y; ++i) {
        const spotify::QueueItem& item = queue[i];
        const float a = i == 0 ? 1.0f : 0.78f;
        if (i == 0) {
            draw->AddRectFilled(ImVec2(min.x, y - fs * 0.25f), ImVec2(max.x, y + rowH - fs * 0.45f), U(p.surface, 0.55f),
                                fs * 0.5f);
        }
        char number[8];
        std::snprintf(number, sizeof(number), "%zu", i + 1);
        draw->AddText(mid, mid->FontSize, ImVec2(min.x + fs * 0.6f, y + fs * 0.35f), U(p.textDim, a), number);
        char length[16];
        FormatTime(static_cast<double>(item.seconds), length, sizeof(length));
        const float lw = reg->CalcTextSizeA(reg->FontSize, FLT_MAX, 0.0f, length).x;
        draw->AddText(reg, reg->FontSize, ImVec2(max.x - fs * 0.6f - lw, y + fs * 0.45f), U(p.textDim, a), length);
        const float textX = min.x + fs * 2.2f;
        const float textW = max.x - fs * 1.4f - lw - textX;
        char title[200], artist[200];
        Fit(mid, mid->FontSize, item.title.c_str(), textW, title, sizeof(title));
        Fit(reg, reg->FontSize, item.artist.c_str(), textW, artist, sizeof(artist));
        draw->AddText(mid, mid->FontSize, ImVec2(textX, y), U(p.text, a), title);
        draw->AddText(reg, reg->FontSize, ImVec2(textX, y + mid->FontSize + fs * 0.1f), U(p.textDim, a), artist);
        y += rowH;
    }
}

// Who draws Lyric Flow and the visualizer (render/world_draw.h). One choice
// for both, shown in both sections.
bool DrawWorldDrawChoice() {
    Settings& s = Get();
    const bool changed = ui::Choice("Drawn by", &s.worldDraw, {"Luma (always visible)", "The game (the terrain covers it)"});
    if (s.worldDraw == 0) {
        ui::Hint("Luma draws it on top of the game: it looks the same with any client or texture pack, but mountains "
                 "and walls in front don't cover it.");
    } else {
        ui::Hint("The game draws it as part of the world, with the block outline's look: other clients or texture "
                 "packs that change the outline change it too.");
    }
    return changed;
}

void DrawWorldEffects() {
    const Settings& s = Get();
    PollMusicKeys();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (s.active && s.lyricFlow) {
        lyrics::Want(lyrics::kHud, true);
        render::lyricflow::Draw(ImGui::GetForegroundDrawList(), display);
    } else {
        render::lyricflow::Stop();
    }
    if (s.active && VisualizerOn()) {
        render::visualizer::Draw(display);
    } else {
        render::visualizer::Stop();
    }
    // If Luma draws them (and not the game), now: behind the HUD and the menu.
    // Not while a screen of the GAME is open (inventory, chat, pause...): the
    // game draws those on top of the world, so the lyrics must not cover them.
    // (With Luma's own menu open they stay: the menu is drawn above them.)
    const bool gameScreen = sdk::mouse::GameMouseState() == 0 && !sdk::mouse::ReleasedByLuma();
    if (!gameScreen) render::worlddraw::Draw(ImGui::GetBackgroundDrawList(), display);
}

// THE MUSIC KEYS WHILE PLAYING: pressed once (not held), only while you're
// really playing (the game has the mouse: not in the chat, the inventory or
// a menu, so typing a letter never skips a song) and Luma's menu is closed
// (in the menu, Space and the arrows do it: render/menu.cpp).
void PollMusicKeys() {
    const Settings& s = Get();
    static bool wasDown[3] = {true, true, true};  // (a key held while Luma loads isn't a press)
    const int keys[3] = {s.keyPlayPause, s.keyNext, s.keyPrevious};
    const bool playing = sdk::mouse::GameMouseState() == 1 && !overlay::IsMenuOpen();
    for (int i = 0; i < 3; ++i) {
        const bool down = keys[i] != 0 && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !wasDown[i] && playing) {
            if (i == 0) media::PlayPause();
            if (i == 1) media::Next();
            if (i == 2) media::Previous();
        }
        wasDown[i] = down;
    }
}

bool DrawMusicKeySettings() {
    Settings& s = Get();
    ui::Key("Play / pause", &s.keyPlayPause);
    ui::Key("Next song", &s.keyNext);
    ui::Key("Previous song", &s.keyPrevious);
    ui::Hint("While you play (not in the chat or the inventory). None by default: click a key to choose one, right click to "
             "remove it. In Luma's menu: Space plays or pauses, the left and right arrows change the song.");
    return false;
}

bool VisualizerOn() {
    const Settings& s = Get();
    return s.visualizer && !(s.efficiency && s.effVisualizer);
}

bool DrawVisualizerSettings() {
    ui::Section("Visualizer in the sky");
    return DrawWholePage(kPageVisualizer);
}

namespace {
// The Client ID box and the connect button (without the long help).
bool DrawSpotifyConnect(Settings& s) {
    bool changed = false;
    ImGui::PushID("spotify");
    struct PopId {
        ~PopId() { ImGui::PopID(); }
    } popId;
    static char clientId[72] = "";
    static bool loaded = false;
    if (!loaded) {
        std::snprintf(clientId, sizeof(clientId), "%s", s.spotifyClientId.c_str());
        loaded = true;
    }
    if (ui::Text("Client ID", clientId, sizeof(clientId), "paste it from developer.spotify.com")) {
        // Only letters and numbers (a Client ID is 32 of them).
        std::string clean;
        for (const char* c = clientId; *c != '\0'; ++c) {
            if (std::isalnum(static_cast<unsigned char>(*c))) clean.push_back(*c);
        }
        if (clean != s.spotifyClientId) {
            s.spotifyClientId = clean;
            spotify::SetClientId(clean);
            changed = true;
        }
    }
    const spotify::Status status = spotify::GetStatus();
    switch (status.state) {
        case spotify::State::NoClientId:
            ui::Hint("Paste your Client ID above to connect.");
            break;
        case spotify::State::SignedOut:
            if (ui::Button("Connect Spotify", "Your account")) spotify::Connect();
            break;
        case spotify::State::SigningIn:
            ui::Hint("Waiting for you in the browser... (accept there and come back)");
            break;
        case spotify::State::Connected:
            if (ui::Button("Disconnect", "Connected")) spotify::Disconnect();
            break;
        case spotify::State::Error: {
            char text[200];
            std::snprintf(text, sizeof(text), "%s: %s", Tr("Couldn't connect"), status.error.c_str());
            ui::Hint(text);
            if (ui::Button("Try again", "Your account")) spotify::Connect();
            break;
        }
    }
    return changed;
}
}  // namespace

bool DrawSpotifySettings() {
    Settings& s = Get();
    ui::Section("Spotify (optional)");
    const bool changed = DrawSpotifyConnect(s);
    ui::Hint("How to get a Client ID (once, 5 minutes): 1. Go to developer.spotify.com/dashboard and sign in. "
             "2. Create app: any name, Redirect URI exactly http://127.0.0.1:43821/callback, tick \"Web API\". "
             "3. Copy its Client ID here and press Connect. Spotify requires the app's owner to have Premium.");
    ui::Hint("Luma only asks to see what you're playing and your queue. Everything stays on your PC (encrypted); "
             "you can disconnect here or in your Spotify account at any time.");
    return changed;
}

// =============================================================================
//  Settings pages (Music > Settings in the menu; render/menu.cpp draws the
//  cards, the page header and the "More options" switch)
// =============================================================================
//  Every page shows its 3 or 4 most used settings first ("basic") and keeps
//  the fine ones in "More options", split in small tabs. The old functions
//  (DrawStyleSettings...) still exist for the module's own panel: they draw
//  a page whole (basic + every tab).

namespace {

// The tabs of "More options", per page (English keys: Tr() translates them).
constexpr const char* kTabsOnScreen[] = {"Look"};
constexpr const char* kTabsLyrics[] = {"Sources", "On the HUD"};
constexpr const char* kTabsFlow[] = {"Place", "Letters", "Animation", "Look", "Effects"};
constexpr const char* kTabsViz[] = {"Place", "Shape", "Reaction", "Look"};
constexpr const char* kTabsHalo[] = {"Ring"};
constexpr const char* kTabsColors[] = {"Album art"};
constexpr const char* kTabsAccount[] = {"Spotify help", "Lighter mode"};

// --- READY-MADE STYLES ("presets") ----------------------------------------------------
//  A style changes only how it LOOKS (colors, effects, animations): never
//  where the lyrics are, their size or their font. Once applied, everything
//  can still be changed by hand; the style's button stays lit only while
//  nothing of it was changed.
struct FlowLook {
    int colorMode;
    float color[3], color2[3];
    bool outline;
    bool shadow;
    int shadowStyle;
    int appear, leave, motion;
    float speed;
    int fill, unsung, glow, glowColor;
    float glowStrength;
    int glowWhere, vfx;
    float vfxAmount;
    int vfxColor;
};
constexpr const char* kFlowStyleNames[] = {"Karaoke", "Neon", "Party", "Dream", "Fire", "Minimal"};
constexpr FlowLook kFlowStyles[] = {
    // Karaoke: the album's color, filled as it's sung, a soft glow on the word being sung.
    {1, {1, 1, 1}, {0.70f, 0.56f, 1.0f}, true, false, 0, 1, 1, 0, 1.0f, 1, 0, 1, 0, 1.0f, 1, 0, 1.0f, 0},
    // Neon: one strong color, a neon glow on every word, a bright edge and sparks.
    {0, {0.30f, 0.95f, 1.0f}, {0.70f, 0.56f, 1.0f}, false, false, 0, 0, 1, 0, 1.0f, 4, 0, 3, 0, 1.2f, 0, 1, 1.0f, 0},
    // Party: a rainbow, words that pop and wave, confetti, a glow that pulses.
    {4, {1, 1, 1}, {0.70f, 0.56f, 1.0f}, true, true, 0, 0, 4, 2, 1.0f, 2, 0, 5, 3, 1.0f, 1, 3, 1.2f, 2},
    // Dream: the album's gradient, a wide light, stardust, a colored shadow, floating.
    {2, {1, 1, 1}, {0.70f, 0.56f, 1.0f}, false, true, 1, 3, 0, 1, 0.8f, 1, 0, 2, 1, 0.9f, 0, 2, 1.0f, 1},
    // Fire: orange to yellow, a flash on each word, embers, thick 3D letters.
    {3, {1.0f, 0.45f, 0.10f}, {1.0f, 0.85f, 0.30f}, true, true, 2, 2, 2, 0, 1.0f, 1, 0, 4, 0, 1.0f, 1, 4, 1.0f, 0},
    // Minimal: white with a border, nothing else.
    {0, {1, 1, 1}, {0.70f, 0.56f, 1.0f}, true, false, 0, 1, 1, 0, 1.0f, 0, 0, 0, 0, 1.0f, 1, 0, 1.0f, 0},
};
constexpr int kFlowStyleCount = static_cast<int>(sizeof(kFlowStyles) / sizeof(kFlowStyles[0]));

bool Near(float a, float b) {
    return std::fabs(a - b) < 0.001f;
}
bool Near3(const float* a, const float* b) {
    return Near(a[0], b[0]) && Near(a[1], b[1]) && Near(a[2], b[2]);
}

void ApplyFlowStyle(Settings& s, int i) {
    const FlowLook& k = kFlowStyles[i];
    s.flowColorMode = k.colorMode;
    std::memcpy(s.flowColor, k.color, sizeof(k.color));
    std::memcpy(s.flowColor2, k.color2, sizeof(k.color2));
    s.flowOutline = k.outline;
    s.flowShadow = k.shadow;
    s.flowShadowStyle = k.shadowStyle;
    s.flowAppear = k.appear;
    s.flowLeave = k.leave;
    s.flowMotion = k.motion;
    s.flowSpeed = k.speed;
    s.flowFill = k.fill;
    s.flowUnsung = k.unsung;
    s.flowGlow = k.glow;
    s.flowGlowColor = k.glowColor;
    s.flowGlowStrength = k.glowStrength;
    s.flowGlowWhere = k.glowWhere;
    s.flowVfx = k.vfx;
    s.flowVfxAmount = k.vfxAmount;
    s.flowVfxColor = k.vfxColor;
    // (and the defaults of what the styles don't choose)
    const Settings d{};
    s.flowHighlight = false;
    s.flowOpacity = d.flowOpacity;
    std::memcpy(s.flowOutlineColor, d.flowOutlineColor, sizeof(d.flowOutlineColor));
    s.flowOutlineAlpha = d.flowOutlineAlpha;
}

int CurrentFlowStyle(const Settings& s) {
    const Settings d{};
    if (s.flowHighlight || !Near(s.flowOpacity, d.flowOpacity) || !Near(s.flowOutlineAlpha, d.flowOutlineAlpha) ||
        !Near3(s.flowOutlineColor, d.flowOutlineColor)) {
        return -1;
    }
    for (int i = 0; i < kFlowStyleCount; ++i) {
        const FlowLook& k = kFlowStyles[i];
        const bool colors = k.colorMode != 0 && k.colorMode != 3 ? true
                            : (Near3(s.flowColor, k.color) && (k.colorMode != 3 || Near3(s.flowColor2, k.color2)));
        if (s.flowColorMode == k.colorMode && colors && s.flowOutline == k.outline && s.flowShadow == k.shadow &&
            (!k.shadow || s.flowShadowStyle == k.shadowStyle) && s.flowAppear == k.appear && s.flowLeave == k.leave &&
            s.flowMotion == k.motion && Near(s.flowSpeed, k.speed) && s.flowFill == k.fill && s.flowUnsung == k.unsung &&
            s.flowGlow == k.glow && (k.glow == 0 || (s.flowGlowColor == k.glowColor && Near(s.flowGlowStrength, k.glowStrength) &&
                                                     s.flowGlowWhere == k.glowWhere)) &&
            s.flowVfx == k.vfx && (k.vfx == 0 || (Near(s.flowVfxAmount, k.vfxAmount) && s.flowVfxColor == k.vfxColor))) {
            return i;
        }
    }
    return -1;
}

struct VizLook {
    int style, colorMode;
    bool layers, reflection;
    float thickness, opacity, hitReaction, smoothing, spin;
};
constexpr const char* kVizStyleNames[] = {"Bars", "Aurora", "Rings", "Soft wave"};
constexpr VizLook kVizStyles[] = {
    {0, 1, false, true, 0.6f, 0.85f, 0.6f, 0.5f, 0.0f},   // Bars: the album's color, a reflection below
    {3, 5, true, false, 0.8f, 0.7f, 0.4f, 0.7f, 2.0f},    // Aurora: curtains of light in the album's palette
    {4, 4, true, false, 0.5f, 0.85f, 1.0f, 0.4f, 6.0f},   // Rings: a turning rainbow that jumps with the hits
    {6, 2, true, true, 0.5f, 0.8f, 0.5f, 0.75f, 0.0f},    // Soft wave: layered waves in the album's gradient
};
constexpr int kVizStyleCount = static_cast<int>(sizeof(kVizStyles) / sizeof(kVizStyles[0]));

void ApplyVizStyle(Settings& s, int i) {
    const VizLook& k = kVizStyles[i];
    s.vizStyle = k.style;
    s.vizColorMode = k.colorMode;
    s.vizLayers = k.layers;
    s.vizReflection = k.reflection;
    s.vizThickness = k.thickness;
    s.vizOpacity = k.opacity;
    s.vizHitReaction = k.hitReaction;
    s.vizSmoothing = k.smoothing;
    s.vizSpin = k.spin;
}

int CurrentVizStyle(const Settings& s) {
    for (int i = 0; i < kVizStyleCount; ++i) {
        const VizLook& k = kVizStyles[i];
        if (s.vizStyle == k.style && s.vizColorMode == k.colorMode && s.vizLayers == k.layers && s.vizReflection == k.reflection &&
            Near(s.vizThickness, k.thickness) && Near(s.vizOpacity, k.opacity) && Near(s.vizHitReaction, k.hitReaction) &&
            Near(s.vizSmoothing, k.smoothing) && Near(s.vizSpin, k.spin)) {
            return i;
        }
    }
    return -1;
}

// --- On screen ----------------------------------------------------------------------
bool OnScreenBasic(Settings& s) {
    bool changed = false;
    changed |= ui::Choice("Style", &s.hudStyle, {"Compact", "With progress", "Card", "One line", "Vinyl", "Lyrics", "Wide + lyrics"});
    changed |= ui::Toggle("Album art", &s.showArt);
    if (s.showArt) {
        changed |= ui::Choice("Art shape", &s.artShape, {"Square", "Rounded", "Round"});
        if (s.artShape == 2) changed |= ui::Toggle("Spin while playing", &s.spinArt);
    }
    changed |= ui::Toggle("Artist", &s.showArtist);
    changed |= ui::Toggle("Album", &s.showAlbum);
    changed |= ui::Toggle("Time", &s.showTime);
    return changed;
}
bool OnScreenMore(Settings& s) {
    bool changed = false;
    changed |= ui::Choice("Accent color", &s.accent, {"Like the text", "Album colors", "Rainbow"});
    changed |= ui::Toggle("App (Spotify, Chrome...)", &s.showApp);
    changed |= ui::SliderInt("Max width", &s.maxWidth, 60, 320, "%d");
    changed |= ui::Toggle("Scroll long names", &s.marquee);
    changed |= ui::Toggle("Fade in new songs", &s.animateChanges);
    changed |= ui::Toggle("Hide when paused", &s.hideWhenPaused);
    changed |= ui::Toggle("Hide when nothing plays", &s.hideWhenNothing);
    return changed;
}

// --- Lyrics -------------------------------------------------------------------------
bool LyricsBasic(Settings& s) {
    bool changed = false;
    if (ui::Toggle("Search lyrics online", &s.lyricsOnline)) {
        lyrics::SetOnline(s.lyricsOnline);
        changed = true;
    }
    changed |= ui::Slider("Offset (seconds)", &s.lyricsOffset, -5.0f, 5.0f, "%+.1f");
    ui::Hint("If the lyrics come early or late, move the offset.");
    return changed;
}
bool LyricsMore(Settings& s, int tab) {
    bool changed = false;
    if (tab == 0) {
        if (ui::Toggle("Advanced source: LiriQo", &s.lyricsLiriqo, "(word by word, third-party service)")) {
            lyrics::SetLiriqo(s.lyricsLiriqo);
            changed = true;
        }
        ui::Hint("To find the lyrics, the song's title, artist, album and length are sent to LiriQo (a free service run "
                 "by one developer, which asks Apple Music, NetEase and others) and, if it doesn't have them, to LRCLIB "
                 "(lrclib.net). Nothing about you. Found lyrics are saved on your PC, so each song is looked up once.");
    } else {
        changed |= ui::Toggle("Next line on the HUD", &s.lyricsNextLine);
        changed |= ui::Toggle("Smooth word sweep on the HUD", &s.lyricsSweep);
    }
    return changed;
}

// --- Lyric Flow ---------------------------------------------------------------------
bool FlowBasic(Settings& s) {
    bool changed = false;
    changed |= ui::Toggle("Lyrics in the sky", &s.lyricFlow);
    if (!s.lyricFlow) {
        ui::Hint("Each line is written in the world as it's sung, with real pixel letters. Best with word-by-word lyrics.");
        return changed;
    }
    {
        const int picked = ui::Presets({kFlowStyleNames[0], kFlowStyleNames[1], kFlowStyleNames[2], kFlowStyleNames[3],
                                        kFlowStyleNames[4], kFlowStyleNames[5]},
                                       CurrentFlowStyle(s));
        if (picked >= 0) {
            ApplyFlowStyle(s, picked);
            changed = true;
        }
    }
    if (ui::WideButton("Adjust in the world")) menu::StartLiveEdit(kPageLyricFlow);
    changed |= ui::Choice("Where", &s.flowMode, {"In the world", "In the sky"});
    ui::Hint(s.flowMode == 1 ? "Very far, like the sun: it follows you when you walk."
                             : "At a place of the world: walk to it and it gets bigger.");
    changed |= ui::Slider("Word size (blocks)", &s.flowSize, 0.5f, 20.0f, "%.1f");
    changed |= ui::Choice("Color", &s.flowColorMode, {"One color", "Album", "Album gradient", "Two colors", "Rainbow"});
    if (s.flowColorMode == 0 || s.flowColorMode == 3) changed |= ui::Color("Color 1", s.flowColor);
    if (s.flowColorMode == 3) changed |= ui::Color("Color 2", s.flowColor2);
    // The three effects (their details: More options > Effects).
    changed |= ui::Choice("Karaoke fill", &s.flowFill, {"Off", "Sweep", "Word at once", "Rise", "Bright edge"});
    changed |= ui::Choice("Glow", &s.flowGlow, {"Off", "Soft", "Bloom", "Neon", "Flash", "Pulse"});
    changed |= ui::Choice("Particles", &s.flowVfx, {"Off", "Sparks", "Stardust", "Confetti", "Embers", "Fireflies"});
    return changed;
}
bool FlowMore(Settings& s, int tab) {
    bool changed = false;
    if (!s.lyricFlow) {
        ui::Hint("Turn on Lyric Flow to change these.");
        return false;
    }
    if (tab == 0) {
        changed |= ui::Choice("Layout", &s.flowLayout, {"Scattered", "Centered", "Left", "Arc", "Stacked", "Staircase"});
        if (s.flowLayout == 0) changed |= ui::Slider("Tilt (degrees)", &s.flowTilt, 0.0f, 30.0f, "%.0f");
        changed |= ui::Slider("Distance (blocks)", &s.flowDistance, 8.0f, 200.0f, "%.0f");
        changed |= ui::Slider("Height (blocks)", &s.flowHeight, -4.0f, 120.0f, "%.0f");
        changed |= ui::Slider("Direction (degrees)", &s.flowAngle, -180.0f, 180.0f, "%.0f");
        ui::Hint("0 = in front, negative = to the left, positive = to the right.");
        changed |= ui::Slider("Side to side", &s.flowSpread, 0.0f, 0.5f, "%.2f");
        changed |= ui::Slider("Turn (degrees)", &s.flowTurn, -90.0f, 90.0f, "%.0f");
        changed |= ui::Slider("Rotation (degrees)", &s.flowRoll, -45.0f, 45.0f, "%.0f");
        ui::Key("Pin the lyrics", &s.flowPinKey);
        changed |= ui::Choice("Pin them", &s.flowPinTarget, {"Where the last line was", "Where I'm looking"});
        if (render::lyricflow::Pinned()) {
            ui::Hint("Pinned now: every line appears in the same place.");
            if (ui::Button("Unpin")) render::lyricflow::SetPinned(false);
        }
    } else if (tab == 1) {
        // The font list (a combo with every font of render/lyric_flow.h).
        if (ui::Matches("Font")) {  // (it follows the search too)
        const char* current = render::lyricflow::kFonts[std::clamp(s.flowFont, 0, render::lyricflow::kFontCount - 1)];
        ImGui::TextUnformatted(Tr("Font"));
        ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##flowFont", current)) {
            for (int i = 0; i < render::lyricflow::kFontCount; ++i) {
                if (ImGui::Selectable(render::lyricflow::kFonts[i], i == s.flowFont)) {
                    s.flowFont = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        }
        if (s.flowFont != 0) {
            changed |= ui::Toggle("Bold", &s.flowBold);
            changed |= ui::SliderInt("Resolution", &s.flowResolution, 9, 32, "%d");
            ui::Hint("More = smoother letters (and a little more work).");
        }
        changed |= ui::Toggle("Uppercase", &s.flowUppercase);
        changed |= ui::Slider("Letter spacing", &s.flowLetterSpacing, -1.0f, 4.0f, "%.1f");
        changed |= ui::Slider("Word spacing", &s.flowWordSpacing, 0.1f, 1.5f, "%.2f");
    } else if (tab == 2) {
        changed |= ui::Choice("Appear", &s.flowAppear,
                              {"Pop", "Fade", "Drop", "Rise", "Zoom", "Spin", "Flip", "Typewriter", "Scatter"});
        changed |= ui::Choice("Leave", &s.flowLeave, {"Float up", "Fade", "Fall", "Shrink", "Scatter"});
        changed |= ui::Choice("Motion", &s.flowMotion, {"None", "Float", "Wave", "Sway", "Pulse"});
        changed |= ui::Slider("Effect speed", &s.flowSpeed, 0.3f, 3.0f, "%.1fx");
        changed |= ui::Slider("Leave time (s)", &s.flowLeaveTime, 0.15f, 1.5f, "%.2f");
        changed |= ui::Slider("Hide when paused (s)", &s.flowPauseHide, 0.0f, 60.0f, "%.0f");
    } else if (tab == 4) {
        ui::Section("Karaoke fill");
        if (s.flowFill == 0) ui::Hint("Choose a fill (Lyric Flow's first options) to light each word up as it's sung.");
        changed |= ui::Toggle("Whole line at once", &s.flowFillWholeLine);
        ui::Hint("On: the whole line is written when it starts and fills as it's sung. Off: each word appears when it's sung.");
        changed |= ui::Choice("Still to sing", &s.flowUnsung, {"Dimmer", "Gray", "White", "My color"});
        if (s.flowUnsung == 3) changed |= ui::Color("Its color", s.flowUnsungColor);
        ui::Section("Glow");
        changed |= ui::Choice("Glow color", &s.flowGlowColor, {"Like the letters", "Album", "My color", "Rainbow"});
        if (s.flowGlowColor == 2) changed |= ui::Color("My glow color", s.flowGlowCustom);
        changed |= ui::Slider("Glow strength", &s.flowGlowStrength, 0.2f, 2.5f, "%.1fx");
        changed |= ui::Choice("Glow on", &s.flowGlowWhere, {"Every word", "Word being sung", "What was sung"});
        ui::Section("Particles");
        changed |= ui::Slider("Amount", &s.flowVfxAmount, 0.1f, 3.0f, "%.1fx");
        changed |= ui::Choice("Particle color", &s.flowVfxColor, {"Like the letters", "Album", "Rainbow", "White"});
        ui::Hint("Glow and particles are more pixels to draw: if your FPS drop, lower the amount or turn them off.");
    } else {
        changed |= ui::Toggle("Highlight the word being sung", &s.flowHighlight);
        if (s.flowHighlight) changed |= ui::Color("Highlight color", s.flowHighlightColor);
        changed |= ui::Slider("Opacity", &s.flowOpacity, 0.2f, 1.0f, "%.2f");
        changed |= ui::Toggle("Outline", &s.flowOutline);
        if (s.flowOutline) {
            changed |= ui::Color("Outline color", s.flowOutlineColor);
            changed |= ui::Slider("Outline opacity", &s.flowOutlineAlpha, 0.1f, 1.0f, "%.2f");
        }
        changed |= ui::Toggle("Shadow", &s.flowShadow);
        if (s.flowShadow) changed |= ui::Choice("Shadow style", &s.flowShadowStyle, {"Dark", "Colored", "3D"});
        changed |= DrawWorldDrawChoice();
    }
    return changed;
}

// --- Visualizer ---------------------------------------------------------------------
bool VizBasic(Settings& s) {
    bool changed = false;
    changed |= ui::Toggle("Visualizer", &s.visualizer);
    if (!s.visualizer) {
        ui::Hint("The sky dances with your music. Luma listens to the sound your PC plays (never the microphone), "
                 "only in memory: nothing is recorded.");
        return changed;
    }
    {
        const int picked = ui::Presets({kVizStyleNames[0], kVizStyleNames[1], kVizStyleNames[2], kVizStyleNames[3]}, CurrentVizStyle(s));
        if (picked >= 0) {
            ApplyVizStyle(s, picked);
            changed = true;
        }
    }
    const audioviz::Levels lv = audioviz::Get();
    if (lv.live) {
        ui::Hint(lv.processOnly ? "Listening to everything except Minecraft (its own sounds don't move it)."
                                : "Listening to all the sound of your PC (this Windows can't leave Minecraft out).");
    }
    changed |= ui::Choice("Style", &s.vizStyle, {"Bars", "Mirrored", "Wave", "Aurora", "Rings", "Pillars", "Layered wave"});
    if (ui::WideButton("Adjust in the world")) menu::StartLiveEdit(kPageVisualizer);
    changed |= ui::Slider("Size (blocks)", &s.vizScale, 4.0f, 80.0f, "%.0f");
    changed |= ui::Choice("Color", &s.vizColorMode, {"One color", "Album", "Album gradient", "Two colors", "Rainbow",
                                                     "Album palette", "Album colors changing", "Album + my color"});
    if (s.vizColorMode == 0 || s.vizColorMode == 3 || s.vizColorMode == 7) {
        changed |= ui::Color(s.vizColorMode == 7 ? "My color" : "Color 1", s.vizColor);
    }
    if (s.vizColorMode == 3) changed |= ui::Color("Color 2", s.vizColor2);
    return changed;
}
bool VizMore(Settings& s, int tab) {
    bool changed = false;
    if (!s.visualizer) {
        ui::Hint("Turn on the visualizer to change these.");
        return false;
    }
    if (tab == 0) {
        changed |= ui::Slider("Distance (blocks)", &s.vizDistance, 30.0f, 200.0f, "%.0f");
        changed |= ui::Toggle("Follow my height", &s.vizFollowHeight);
        if (s.vizFollowHeight) {
            changed |= ui::Slider("Height (blocks)", &s.vizHeight, -30.0f, 60.0f, "%.0f");
        } else {
            changed |= ui::Slider("World height (Y)", &s.vizWorldY, -60.0f, 320.0f, "%.0f");
        }
        changed |= ui::Slider("Spin (degrees/s)", &s.vizSpin, -30.0f, 30.0f, "%.0f");
    } else if (tab == 1) {
        changed |= ui::SliderInt("Bars", &s.vizBars, 24, 256, "%d");
        changed |= ui::Slider("Thickness", &s.vizThickness, 0.1f, 1.0f, "%.2f");
        changed |= ui::Toggle("Layers", &s.vizLayers, "(a soft glow behind and peaks on top)");
        changed |= ui::Toggle("Reflection below", &s.vizReflection);
    } else if (tab == 2) {
        changed |= ui::Slider("Sensitivity", &s.vizSensitivity, 0.3f, 3.0f, "%.1fx");
        changed |= ui::Slider("Reaction to hits", &s.vizHitReaction, 0.0f, 2.0f, "%.1fx");
        changed |= ui::Slider("Smoothing", &s.vizSmoothing, 0.0f, 0.95f, "%.2f");
    } else {
        changed |= ui::Slider("Opacity", &s.vizOpacity, 0.1f, 1.0f, "%.2f");
        if (s.vizColorMode >= 5 && s.vizColorMode <= 6) {
            ui::Hint("The palette is taken from the album art: its most present colors.");
        }
        changed |= DrawWorldDrawChoice();
    }
    return changed;
}

// --- Halo ---------------------------------------------------------------------------
bool HaloBasic(Settings& s) {
    bool changed = false;
    if (ui::Toggle("Show the Halo", &s.halo)) {
        media::Want(media::kRadial, s.halo && s.active);
        changed = true;
    }
    if (!s.halo) {
        ui::Hint("A ring with the song around Luma's radial menu.");
        return changed;
    }
    changed |= ui::Choice("Halo color", &s.haloColor, {"Luma's accent", "Album colors", "Rainbow"});
    changed |= ui::Choice("Buttons", &s.haloButtons, {"Under the ring", "Around it", "None"});
    changed |= ui::Toggle("Album art in the middle", &s.haloArtCenter);
    if (s.haloArtCenter) changed |= ui::Choice("Middle art shape", &s.haloArtShape, {"Square", "Rounded", "Round"});
    return changed;
}
bool HaloMore(Settings& s) {
    if (!s.halo) {
        ui::Hint("Turn on the Halo to change these.");
        return false;
    }
    bool changed = false;
    changed |= ui::Slider("Ring thickness", &s.haloThickness, 0.1f, 1.0f, "%.2f");
    changed |= ui::Slider("Distance from the radial", &s.haloGap, 0.5f, 4.0f, "%.1f");
    changed |= ui::Toggle("Dim full ring", &s.haloTrack);
    changed |= ui::Toggle("Position dot (drag to seek)", &s.haloKnob);
    changed |= ui::Toggle("Title and artist", &s.haloTitle);
    return changed;
}

// --- Album colors -------------------------------------------------------------------
bool ColorsBasic(Settings& s) {
    bool changed = false;
    // (In the music-only Luma there's no other HUD, crosshair or watermark of
    // Luma to color: only the menu.)
    if (!features::kMusicOnly) changed |= ui::Toggle("Color the whole HUD", &s.hudFollowAlbum);
    changed |= ui::Toggle("Color Luma's menu", &s.menuFollowAlbum);
    if (!features::kMusicOnly) {
        changed |= ui::Toggle("Color the crosshair", &s.crosshairAlbum);
        changed |= ui::Toggle("Color Luma's watermark", &s.watermarkAlbum);
    }
    ui::Hint("They take the color of the album art of the song that's playing.");
    return changed;
}
bool ColorsMore(Settings& s) {
    bool changed = false;
    if (ui::Toggle("HD album art", &s.hdCovers, "(when the app's is small)")) {
        ApplyEfficiency();
        changed = true;
    }
    ui::Hint("Some apps give Windows a tiny album art (Liner: 150x150). Luma then looks the song up in Apple's "
             "iTunes catalog (only its title and artist) and saves the sharp art on your PC.");
    return changed;
}

// --- Spotify and performance --------------------------------------------------------
bool AccountBasic(Settings& s) {
    bool changed = DrawSpotifyConnect(s);
    ui::Section("Performance");
    if (ui::Toggle("Music efficiency mode", &s.efficiency)) {
        ApplyEfficiency();
        changed = true;
    }
    ui::Hint("Turns off the music's extras (animations, album colors, background downloads). The music and its "
             "lyrics keep working.");
    return changed;
}
bool AccountMore(Settings& s, int tab) {
    bool changed = false;
    if (tab == 0) {
        ui::Hint("How to get a Client ID (once, 5 minutes): 1. Go to developer.spotify.com/dashboard and sign in. "
                 "2. Create app: any name, Redirect URI exactly http://127.0.0.1:43821/callback, tick \"Web API\". "
                 "3. Copy its Client ID here and press Connect. Spotify requires the app's owner to have Premium.");
        ui::Hint("Luma only asks to see what you're playing and your queue. Everything stays on your PC (encrypted); "
                 "you can disconnect here or in your Spotify account at any time.");
        return false;
    }
    ui::Hint("What the efficiency mode turns off:");
    ImGui::BeginDisabled(!s.efficiency);
    changed |= ui::Toggle("Lyric animations", &s.effLyricAnim);
    changed |= ui::Toggle("Music page animations", &s.effUiAnim);
    changed |= ui::Toggle("Spinning art", &s.effSpin);
    changed |= ui::Toggle("Song change fade", &s.effSongFade);
    changed |= ui::Toggle("Scrolling names", &s.effMarquee);
    changed |= ui::Toggle("Album colors", &s.effAlbumColors);
    changed |= ui::Toggle("HD album art", &s.effHdCovers);
    changed |= ui::Toggle("Lyrics ahead (Spotify)", &s.effPrefetch);
    changed |= ui::Toggle("Visualizer", &s.effVisualizer);
    ImGui::EndDisabled();
    if (changed) ApplyEfficiency();
    return changed;
}

template <std::size_t N>
int Tabs(const char* const (&tabs)[N], const char* const** out) {
    *out = tabs;
    return static_cast<int>(N);
}

}  // namespace

const char* PageTitle(int page) {
    static constexpr const char* kTitles[kPageCount] = {"On screen", "Lyrics", "Lyric Flow", "Visualizer",
                                                        "Halo",      "Album colors", "Spotify and performance"};
    return kTitles[std::clamp(page, 0, kPageCount - 1)];
}

int PageMoreTabs(int page, const char* const** names) {
    switch (page) {
        case kPageOnScreen: return Tabs(kTabsOnScreen, names);
        case kPageLyrics: return Tabs(kTabsLyrics, names);
        case kPageLyricFlow: return Tabs(kTabsFlow, names);
        case kPageVisualizer: return Tabs(kTabsViz, names);
        case kPageHalo: return Tabs(kTabsHalo, names);
        case kPageColors: return Tabs(kTabsColors, names);
        default: return Tabs(kTabsAccount, names);
    }
}

void PageSummary(int page, char* out, std::size_t size) {
    const Settings& s = Get();
    const char* text = "";
    switch (page) {
        case kPageOnScreen: {
            static constexpr const char* kStyles[] = {"Compact", "With progress", "Card", "One line",
                                                      "Vinyl",   "Lyrics",        "Wide + lyrics"};
            text = kStyles[std::clamp(s.hudStyle, 0, 6)];
            break;
        }
        case kPageLyrics: text = s.lyricsOnline ? "Searched online" : "Only saved ones"; break;
        case kPageLyricFlow: text = !s.lyricFlow ? "Off" : (s.flowMode == 1 ? "In the sky" : "In the world"); break;
        case kPageVisualizer: {
            static constexpr const char* kStyles[] = {"Bars", "Mirrored", "Wave", "Aurora", "Rings", "Pillars", "Layered wave"};
            text = s.visualizer ? kStyles[std::clamp(s.vizStyle, 0, 6)] : "Off";
            break;
        }
        case kPageHalo: text = s.halo ? "On" : "Off"; break;
        case kPageColors: {
            const int n = features::kMusicOnly ? (s.menuFollowAlbum ? 1 : 0)
                                               : (s.hudFollowAlbum ? 1 : 0) + (s.menuFollowAlbum ? 1 : 0) +
                                                     (s.crosshairAlbum ? 1 : 0) + (s.watermarkAlbum ? 1 : 0);
            if (features::kMusicOnly && n == 1) {
                text = "Luma's menu";
            } else if (n == 0) {
                text = "Off";
            } else {
                std::snprintf(out, size, Tr(n == 1 ? "%d place" : "%d places"), n);
                return;
            }
            break;
        }
        default: {
            const bool connected = spotify::GetStatus().state == spotify::State::Connected;
            std::snprintf(out, size, "%s%s%s", Tr(connected ? "Spotify connected" : "Spotify not connected"),
                          s.efficiency ? " · " : "", s.efficiency ? Tr("Lighter mode") : "");
            return;
        }
    }
    std::snprintf(out, size, "%s", Tr(text));
}

bool DrawPageBasic(int page) {
    Settings& s = Get();
    ImGui::PushID(page);
    bool changed = false;
    switch (page) {
        case kPageOnScreen: changed = OnScreenBasic(s); break;
        case kPageLyrics: changed = LyricsBasic(s); break;
        case kPageLyricFlow: changed = FlowBasic(s); break;
        case kPageVisualizer: changed = VizBasic(s); break;
        case kPageHalo: changed = HaloBasic(s); break;
        case kPageColors: changed = ColorsBasic(s); break;
        default: changed = AccountBasic(s); break;
    }
    ImGui::PopID();
    return changed;
}

bool DrawPageMore(int page, int tab) {
    Settings& s = Get();
    ImGui::PushID(page + 100);
    ImGui::PushID(tab);
    bool changed = false;
    switch (page) {
        case kPageOnScreen: changed = OnScreenMore(s); break;
        case kPageLyrics: changed = LyricsMore(s, tab); break;
        case kPageLyricFlow: changed = FlowMore(s, tab); break;
        case kPageVisualizer: changed = VizMore(s, tab); break;
        case kPageHalo: changed = HaloMore(s); break;
        case kPageColors: changed = ColorsMore(s); break;
        default: changed = AccountMore(s, tab); break;
    }
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

void ResetPage(int page) {
    Settings& s = Get();
    const Settings d{};
    auto copy3 = [](float* to, const float* from) { std::memcpy(to, from, sizeof(float) * 3); };
    switch (page) {
        case kPageOnScreen:
            s.hudStyle = d.hudStyle; s.artShape = d.artShape; s.spinArt = d.spinArt; s.showArt = d.showArt;
            s.showArtist = d.showArtist; s.showAlbum = d.showAlbum; s.showApp = d.showApp; s.showTime = d.showTime;
            s.accent = d.accent; s.marquee = d.marquee; s.maxWidth = d.maxWidth; s.hideWhenPaused = d.hideWhenPaused;
            s.hideWhenNothing = d.hideWhenNothing; s.animateChanges = d.animateChanges;
            break;
        case kPageLyrics:
            s.lyricsOnline = d.lyricsOnline; s.lyricsLiriqo = d.lyricsLiriqo; s.lyricsOffset = d.lyricsOffset;
            s.lyricsNextLine = d.lyricsNextLine; s.lyricsSweep = d.lyricsSweep;
            lyrics::SetOnline(s.lyricsOnline);
            lyrics::SetLiriqo(s.lyricsLiriqo);
            break;
        case kPageLyricFlow: {
            // Everything of Lyric Flow except whether it's on and the pin key.
            const bool on = s.lyricFlow;
            const int pinKey = s.flowPinKey;
            s.flowDistance = d.flowDistance; s.flowHeight = d.flowHeight; s.flowSize = d.flowSize; s.flowTilt = d.flowTilt;
            s.flowOpacity = d.flowOpacity; s.flowUppercase = d.flowUppercase; s.flowMode = d.flowMode;
            s.flowAngle = d.flowAngle; s.flowSpread = d.flowSpread; s.flowTurn = d.flowTurn; s.flowRoll = d.flowRoll;
            s.flowPinTarget = d.flowPinTarget; s.flowLayout = d.flowLayout; s.flowAppear = d.flowAppear;
            s.flowLeave = d.flowLeave; s.flowMotion = d.flowMotion; s.flowSpeed = d.flowSpeed;
            s.flowColorMode = d.flowColorMode; copy3(s.flowColor, d.flowColor); copy3(s.flowColor2, d.flowColor2);
            s.flowHighlight = d.flowHighlight; copy3(s.flowHighlightColor, d.flowHighlightColor);
            s.flowOutline = d.flowOutline; copy3(s.flowOutlineColor, d.flowOutlineColor);
            s.flowOutlineAlpha = d.flowOutlineAlpha; s.flowShadow = d.flowShadow; s.flowShadowStyle = d.flowShadowStyle; s.flowFont = d.flowFont;
            s.flowBold = d.flowBold; s.flowResolution = d.flowResolution; s.flowLetterSpacing = d.flowLetterSpacing;
            s.flowWordSpacing = d.flowWordSpacing; s.flowLeaveTime = d.flowLeaveTime; s.flowPauseHide = d.flowPauseHide;
            s.worldDraw = d.worldDraw;
            s.flowFill = d.flowFill; s.flowFillWholeLine = d.flowFillWholeLine; s.flowUnsung = d.flowUnsung; copy3(s.flowUnsungColor, d.flowUnsungColor);
            s.flowGlow = d.flowGlow; s.flowGlowColor = d.flowGlowColor; copy3(s.flowGlowCustom, d.flowGlowCustom);
            s.flowGlowStrength = d.flowGlowStrength; s.flowGlowWhere = d.flowGlowWhere;
            s.flowVfx = d.flowVfx; s.flowVfxAmount = d.flowVfxAmount; s.flowVfxColor = d.flowVfxColor;
            s.lyricFlow = on;
            s.flowPinKey = pinKey;
            break;
        }
        case kPageVisualizer: {
            const bool on = s.visualizer;
            s.vizStyle = d.vizStyle; s.vizBars = d.vizBars; s.vizDistance = d.vizDistance; s.vizHeight = d.vizHeight;
            s.vizScale = d.vizScale; s.vizThickness = d.vizThickness; s.vizSensitivity = d.vizSensitivity;
            s.vizSmoothing = d.vizSmoothing; s.vizSpin = d.vizSpin; s.vizColorMode = d.vizColorMode;
            copy3(s.vizColor, d.vizColor); copy3(s.vizColor2, d.vizColor2); s.vizOpacity = d.vizOpacity;
            s.vizFollowHeight = d.vizFollowHeight; s.vizWorldY = d.vizWorldY; s.vizLayers = d.vizLayers;
            s.vizHitReaction = d.vizHitReaction; s.vizReflection = d.vizReflection;
            s.worldDraw = d.worldDraw;
            s.visualizer = on;
            break;
        }
        case kPageHalo: {
            const bool on = s.halo;
            s.haloThickness = d.haloThickness; s.haloGap = d.haloGap; s.haloColor = d.haloColor;
            s.haloTrack = d.haloTrack; s.haloKnob = d.haloKnob; s.haloArtCenter = d.haloArtCenter;
            s.haloArtShape = d.haloArtShape; s.haloTitle = d.haloTitle; s.haloButtons = d.haloButtons;
            s.halo = on;
            break;
        }
        case kPageColors:
            s.hudFollowAlbum = d.hudFollowAlbum; s.menuFollowAlbum = d.menuFollowAlbum;
            s.crosshairAlbum = d.crosshairAlbum; s.watermarkAlbum = d.watermarkAlbum; s.hdCovers = d.hdCovers;
            ApplyEfficiency();
            break;
        default:
            // (The Spotify account stays: disconnecting is its own button.)
            s.efficiency = d.efficiency; s.effLyricAnim = d.effLyricAnim; s.effUiAnim = d.effUiAnim;
            s.effSpin = d.effSpin; s.effSongFade = d.effSongFade; s.effMarquee = d.effMarquee;
            s.effAlbumColors = d.effAlbumColors; s.effHdCovers = d.effHdCovers; s.effPrefetch = d.effPrefetch;
            s.effVisualizer = d.effVisualizer;
            ApplyEfficiency();
            break;
    }
}

bool DrawLivePanel(int page) {
    Settings& s = Get();
    bool changed = false;
    ImGui::PushID("live");
    if (page == kPageVisualizer) {
        changed |= ui::Slider("Distance (blocks)", &s.vizDistance, 30.0f, 200.0f, "%.0f");
        changed |= ui::Toggle("Follow my height", &s.vizFollowHeight);
        if (s.vizFollowHeight) {
            changed |= ui::Slider("Height (blocks)", &s.vizHeight, -30.0f, 60.0f, "%.0f");
        } else {
            changed |= ui::Slider("World height (Y)", &s.vizWorldY, -60.0f, 320.0f, "%.0f");
        }
        changed |= ui::Slider("Size (blocks)", &s.vizScale, 4.0f, 80.0f, "%.0f");
        changed |= ui::Slider("Spin (degrees/s)", &s.vizSpin, -30.0f, 30.0f, "%.0f");
    } else {
        changed |= ui::Choice("Where", &s.flowMode, {"In the world", "In the sky"});
        changed |= ui::Slider("Distance (blocks)", &s.flowDistance, 8.0f, 200.0f, "%.0f");
        changed |= ui::Slider("Height (blocks)", &s.flowHeight, -4.0f, 120.0f, "%.0f");
        changed |= ui::Slider("Direction (degrees)", &s.flowAngle, -180.0f, 180.0f, "%.0f");
        changed |= ui::Slider("Word size (blocks)", &s.flowSize, 0.5f, 20.0f, "%.1f");
        changed |= ui::Slider("Turn (degrees)", &s.flowTurn, -90.0f, 90.0f, "%.0f");
        changed |= ui::Slider("Rotation (degrees)", &s.flowRoll, -45.0f, 45.0f, "%.0f");
        if (render::lyricflow::Pinned()) ui::Hint("Pinned: the lyrics stay where they were pinned.");
    }
    ImGui::PopID();
    return changed;
}

// A whole page (basic + every tab of "More options"), for the module's own panel.
bool DrawWholePage(int page) {
    bool changed = DrawPageBasic(page);
    const char* const* tabs = nullptr;
    const int count = PageMoreTabs(page, &tabs);
    for (int t = 0; t < count; ++t) {
        ui::Section(tabs[t]);
        changed |= DrawPageMore(page, t);
    }
    return changed;
}

}  // namespace luma::render::music
