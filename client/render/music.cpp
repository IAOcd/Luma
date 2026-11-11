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

#include "core/tr.h"
#include "render/gpu.h"
#include "render/icons.h"
#include "render/rainbow.h"
#include "render/theme.h"
#include "render/ui.h"

namespace luma::render::music {

namespace {

constexpr float kPi = 3.14159265f;

// Icons (24x24 SVG paths; "F" at the start = filled).
constexpr const char* kPlayIcon = "FM8 5.5V18.5L18.5 12Z";
constexpr const char* kPauseIcon = "FM7 5H10.5V19H7Z M13.5 5H17V19H13.5Z";
constexpr const char* kNextIcon = "FM6 6V18L14.5 12Z M16 6H18.2V18H16Z";
constexpr const char* kPrevIcon = "FM18 6V18L9.5 12Z M5.8 6H8V18H5.8Z";
constexpr const char* kNoteIcon = "M9 18V6L19 4V16 M9 18C9 19.38 7.88 20.5 6.5 20.5C5.12 20.5 4 19.38 4 18C4 16.62 5.12 15.5 6.5 15.5C7.88 15.5 9 16.62 9 18Z M19 16C19 17.38 17.88 18.5 16.5 18.5C15.12 18.5 14 17.38 14 16C14 14.62 15.12 13.5 16.5 13.5C17.88 13.5 19 14.62 19 16Z";

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
        if (s.active && (s.hudFollowAlbum || s.menuFollowAlbum)) {
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

bool HudTint(float out[3]) {
    const Tint& t = AlbumTint();
    if (!t.has || !Get().hudFollowAlbum) return false;
    std::memcpy(out, t.rgb, sizeof(t.rgb));
    return true;
}

bool MenuTint(float out[3]) {
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
        if (media::Current().playing) {
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
    if (t.canSeek && t.duration > 0.0 && s.haloKnob) {
        const bool nearRing = !overButton && std::fabs(mdist - r) < th * 2.0f + fs * 0.35f;
        if (nearRing) {
            over = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                dragging = true;
            }
        }
        if (dragging) {
            over = true;
            dragFrac = mouseFrac;
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                media::Seek(dragFrac * t.duration);  // drop = jump there
                dragging = false;
            }
        }
    } else {
        dragging = false;
    }
    const float shown = dragging ? dragFrac : frac;
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
        draw->AddCircleFilled(ImVec2(c.x + std::cos(a1) * r, c.y + std::sin(a1) * r), th * (1.35f + 0.6f * grow),
                              U(color, 1.0f), 24);
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
        FormatTime((dragging ? dragFrac : frac) * t.duration, now, sizeof(now));
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
    return static_cast<int>((track.position - static_cast<double>(Get().lyricsOffset)) * 1000.0);
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
        static float scroll = 0.0f;
        static std::uint32_t lastTrack = 0;
        const float target = static_cast<float>(std::max(0, current));
        if (lastTrack != t.trackId) {
            lastTrack = t.trackId;
            scroll = target;
        }
        scroll = ImLerp(scroll, target, std::min(1.0f, ImGui::GetIO().DeltaTime * 6.0f));
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
                // Real word timings: sung words bright, the rest soft.
                float x = min.x, rowY = y;
                for (std::size_t w = 0; w < line.words.size(); ++w) {
                    const lyrics::Word& word = line.words[w];
                    const ImVec2 ws = f->CalcTextSizeA(size, FLT_MAX, 0.0f, word.text.c_str());
                    if (x > min.x && x + ws.x > max.x) {
                        x = min.x;
                        rowY += rowH;
                    }
                    const float on = ms >= word.ms ? 1.0f : 0.38f;
                    draw->AddText(f, size, ImVec2(x, rowY), U(p.text, on), word.text.c_str());
                    x += ws.x;
                }
            } else {
                const char* starts[8];
                const char* ends[8];
                const int rows = WrapRows(f, size, line.text.c_str(), width, starts, ends, 8);
                const float alpha = isCurrent ? 1.0f : (hovered ? soft + 0.25f : soft);
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
        lyrics::SetHdCovers(s.hdCovers);
        lyrics::Want(lyrics::kCovers, s.hdCovers && s.active);
        changed = true;
    }
    ui::Hint("Some apps give Windows a tiny album art (Liner: 150x150). Luma then looks the song up in Apple's "
             "iTunes catalog (only its title and artist) and saves the sharp art on your PC.");
    changed |= ui::Toggle("Color the whole HUD", &s.hudFollowAlbum, "(every module takes the album's color)");
    changed |= ui::Toggle("Color Luma's menu", &s.menuFollowAlbum, "(the accent follows the album)");
    return changed;
}

bool DrawLyricsSettings() {
    Settings& s = Get();
    bool changed = false;
    ui::Section("Lyrics");
    if (ui::Toggle("Search lyrics online", &s.lyricsOnline)) {
        lyrics::SetOnline(s.lyricsOnline);
        changed = true;
    }
    changed |= ui::Slider("Offset (seconds)", &s.lyricsOffset, -5.0f, 5.0f, "%+.1f");
    changed |= ui::Toggle("Next line on the HUD", &s.lyricsNextLine);
    ui::Hint("To find the lyrics, the song's title, artist, album and length are sent to LRCLIB (lrclib.net). "
             "Nothing about you. Found lyrics are saved on your PC, so each song is looked up once.");
    return changed;
}

}  // namespace luma::render::music
