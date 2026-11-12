// =============================================================================
//  music.cpp - How the music looks (see music.h)
// =============================================================================
#include "render/music.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <imgui_internal.h>  // ImLerp for colors

#include "core/spotify.h"
#include "core/tr.h"
#include "render/gpu.h"
#include "render/icons.h"
#include "render/lyric_flow.h"
#include "render/rainbow.h"
#include "render/theme.h"
#include "render/ui.h"

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
    Settings& s = Get();
    bool changed = false;
    ui::Section("On screen");
    changed |= ui::Choice("Style", &s.hudStyle, {"Compact", "With progress", "Card", "One line", "Vinyl", "Lyrics", "Wide + lyrics"});
    changed |= ui::Toggle("Album art", &s.showArt);
    if (s.showArt) {
        changed |= ui::Choice("Art shape", &s.artShape, {"Square", "Rounded", "Round"});
        if (s.artShape == 2) {
            changed |= ui::Toggle("Spin while playing", &s.spinArt);
        }
    }
    changed |= ui::Toggle("Artist", &s.showArtist);
    changed |= ui::Toggle("Album", &s.showAlbum);
    changed |= ui::Toggle("App (Spotify, Chrome...)", &s.showApp);
    changed |= ui::Toggle("Time", &s.showTime);
    changed |= ui::Choice("Accent color", &s.accent, {"Like the text", "Album colors", "Rainbow"});
    changed |= ui::SliderInt("Max width", &s.maxWidth, 60, 320, "%d");
    changed |= ui::Toggle("Scroll long names", &s.marquee);
    changed |= ui::Toggle("Fade in new songs", &s.animateChanges);
    changed |= ui::Toggle("Hide when paused", &s.hideWhenPaused);
    changed |= ui::Toggle("Hide when nothing plays", &s.hideWhenNothing);

    return changed;
}

bool DrawHaloSettings() {
    Settings& s = Get();
    bool changed = false;
    ui::Section("Halo (radial menu)");
    if (ui::Toggle("Show the Halo", &s.halo)) {
        media::Want(media::kRadial, s.halo && s.active);
        changed = true;
    }
    if (s.halo) {
        changed |= ui::Choice("Halo color", &s.haloColor, {"Luma's accent", "Album colors", "Rainbow"});
        changed |= ui::Slider("Ring thickness", &s.haloThickness, 0.1f, 1.0f, "%.2f");
        changed |= ui::Slider("Distance from the radial", &s.haloGap, 0.5f, 4.0f, "%.1f");
        changed |= ui::Toggle("Dim full ring", &s.haloTrack);
        changed |= ui::Toggle("Position dot (drag to seek)", &s.haloKnob);
        changed |= ui::Toggle("Title and artist", &s.haloTitle);
        changed |= ui::Choice("Buttons", &s.haloButtons, {"Under the ring", "Around it", "None"});
        changed |= ui::Toggle("Album art in the middle", &s.haloArtCenter);
        if (s.haloArtCenter) {
            changed |= ui::Choice("Middle art shape", &s.haloArtShape, {"Square", "Rounded", "Round"});
        }
    }
    return changed;
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
    Settings& s = Get();
    bool changed = false;
    ui::Section("Album colors");
    if (ui::Toggle("HD album art", &s.hdCovers, "(when the app's is small)")) {
        ApplyEfficiency();
        changed = true;
    }
    ui::Hint("Some apps give Windows a tiny album art (Liner: 150x150). Luma then looks the song up in Apple's "
             "iTunes catalog (only its title and artist) and saves the sharp art on your PC.");
    changed |= ui::Toggle("Color the whole HUD", &s.hudFollowAlbum, "(every module takes the album's color)");
    changed |= ui::Toggle("Color Luma's menu", &s.menuFollowAlbum, "(the accent follows the album)");
    changed |= ui::Toggle("Color the crosshair", &s.crosshairAlbum);
    changed |= ui::Toggle("Color Luma's watermark", &s.watermarkAlbum);
    return changed;
}

bool DrawLyricsSettings() {
    Settings& s = Get();
    bool changed = false;
    ui::Section("Lyric Flow");
    changed |= ui::Toggle("Lyrics in the sky", &s.lyricFlow, "(each word written in the world as it's sung)");
    if (s.lyricFlow) {
        changed |= ui::Choice("Where", &s.flowMode, {"In the world", "In the sky"});
        if (s.flowMode == 1) {
            ui::Hint("In the sky: very far, like the sun or the clouds. It follows you when you walk (you never get "
                     "closer), but stays put when you turn: it's part of the sky. Everything in front covers it.");
        }
        changed |= ui::Choice("Layout", &s.flowLayout, {"Scattered", "Centered", "Left", "Arc", "Stacked", "Staircase"});
        changed |= ui::Slider("Distance (blocks)", &s.flowDistance, 8.0f, 60.0f, "%.0f");
        changed |= ui::Slider("Height (blocks)", &s.flowHeight, -4.0f, 40.0f, "%.0f");
        changed |= ui::Slider("Word size (blocks)", &s.flowSize, 0.5f, 6.0f, "%.1f");
        if (s.flowLayout == 0) changed |= ui::Slider("Tilt (degrees)", &s.flowTilt, 0.0f, 30.0f, "%.0f");
        ui::Section("Lyric Flow: letters");
        {
            // The font list (a combo with every font of render/lyric_flow.h).
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
            ui::Hint("How many pixels tall a letter is: more = smoother letters (and a little more work).");
        }
        changed |= ui::Toggle("Uppercase", &s.flowUppercase);
        changed |= ui::Slider("Letter spacing", &s.flowLetterSpacing, -1.0f, 4.0f, "%.1f");
        changed |= ui::Slider("Word spacing", &s.flowWordSpacing, 0.1f, 1.5f, "%.2f");
        ui::Section("Lyric Flow: effects");
        changed |= ui::Choice("Appear", &s.flowAppear,
                              {"Pop", "Fade", "Drop", "Rise", "Zoom", "Spin", "Flip", "Typewriter", "Scatter"});
        changed |= ui::Choice("Leave", &s.flowLeave, {"Float up", "Fade", "Fall", "Shrink", "Scatter"});
        changed |= ui::Choice("Motion", &s.flowMotion, {"None", "Float", "Wave", "Sway", "Pulse"});
        changed |= ui::Slider("Effect speed", &s.flowSpeed, 0.3f, 3.0f, "%.1fx");
        changed |= ui::Slider("Leave time (s)", &s.flowLeaveTime, 0.15f, 1.5f, "%.2f");
        ui::Hint("A line starts leaving just before the next one arrives and is gone when it appears: they never overlap.");
        ui::Section("Lyric Flow: colors");
        changed |= ui::Choice("Color", &s.flowColorMode, {"One color", "Album", "Album gradient", "Two colors", "Rainbow"});
        if (s.flowColorMode == 0 || s.flowColorMode == 3) changed |= ui::Color("Color 1", s.flowColor);
        if (s.flowColorMode == 3) changed |= ui::Color("Color 2", s.flowColor2);
        changed |= ui::Toggle("Highlight the word being sung", &s.flowHighlight);
        if (s.flowHighlight) changed |= ui::Color("Highlight color", s.flowHighlightColor);
        changed |= ui::Slider("Opacity", &s.flowOpacity, 0.2f, 1.0f, "%.2f");
        changed |= ui::Toggle("Outline", &s.flowOutline, "(a border around the letters)");
        if (s.flowOutline) {
            changed |= ui::Color("Outline color", s.flowOutlineColor);
            changed |= ui::Slider("Outline opacity", &s.flowOutlineAlpha, 0.1f, 1.0f, "%.2f");
        }
        changed |= ui::Toggle("Shadow", &s.flowShadow, "(a dark copy behind)");
    }
    ui::Hint("Each line is written in the sky in front of where you look, as real pixel letters in the world: "
             "turn your head and it stays there, and mountains in front cover it. Best with word-by-word lyrics.");
    ui::Section("Lyrics");
    if (ui::Toggle("Search lyrics online", &s.lyricsOnline)) {
        lyrics::SetOnline(s.lyricsOnline);
        changed = true;
    }
    if (ui::Toggle("Advanced source: LiriQo", &s.lyricsLiriqo, "(word by word, third-party service)")) {
        lyrics::SetLiriqo(s.lyricsLiriqo);
        changed = true;
    }
    changed |= ui::Slider("Offset (seconds)", &s.lyricsOffset, -5.0f, 5.0f, "%+.1f");
    changed |= ui::Toggle("Next line on the HUD", &s.lyricsNextLine);
    changed |= ui::Toggle("Smooth word sweep on the HUD", &s.lyricsSweep, "(off: each word lights up at once)");
    ui::Hint("To find the lyrics, the song's title, artist, album and length are sent to LiriQo (a free service run "
             "by one developer, which asks Apple Music, NetEase and others) and, if it doesn't have them, to LRCLIB "
             "(lrclib.net). Nothing about you. Found lyrics are saved on your PC, so each song is looked up once.");
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

bool DrawSpotifySettings() {
    Settings& s = Get();
    bool changed = false;
    ui::Section("Spotify (optional)");
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
    ui::Hint("How to get a Client ID (once, 5 minutes): 1. Go to developer.spotify.com/dashboard and sign in. "
             "2. Create app: any name, Redirect URI exactly http://127.0.0.1:43821/callback, tick \"Web API\". "
             "3. Copy its Client ID here and press Connect. Spotify requires the app's owner to have Premium.");
    ui::Hint("Luma only asks to see what you're playing and your queue. Everything stays on your PC (encrypted); "
             "you can disconnect here or in your Spotify account at any time.");
    return changed;
}

}  // namespace luma::render::music
