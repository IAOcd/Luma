// =============================================================================
//  music.cpp - Module "Now playing" (see music.h)
// =============================================================================
#include "modules/hud/music.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/lyrics.h"
#include "core/media.h"
#include "render/hud.h"
#include "render/music.h"
#include "render/rainbow.h"
#include "render/ui.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace ui = render::ui;
namespace mu = render::music;

namespace {

// Lyrics lines are longer than titles: they get twice the max width.
float maxTextWidthForLyrics(const mu::Settings& s, float u) {
    return static_cast<float>(s.maxWidth) * 2.0f * u;
}

ImU32 WithAlpha(ImU32 color, float alpha) {
    const ImU32 a = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// One line of text that fits in "width": long ones scroll (marquee) or get cut.
void Line(ImDrawList* draw, const rh::Metrics& m, ImVec2 pos, float width, const char* text, ImU32 color,
          bool marquee, float centerIn = 0.0f) {
    const float w = rh::TextSize(m, text).x;
    const ImVec2 clipMin(pos.x, pos.y - m.capHeight);
    const ImVec2 clipMax(pos.x + width, pos.y + m.capHeight * 3.0f);
    draw->PushClipRect(clipMin, clipMax, true);
    if (w <= width) {
        const float x = centerIn > 0.0f ? pos.x + (width - w) * 0.5f : pos.x;
        rh::Text(draw, m, ImVec2(x, pos.y - m.capTop), text, color);
    } else if (marquee) {
        const float gap = m.px * 24.0f;
        const float speed = m.px * 18.0f;  // HUD pixels per second
        const float cycle = w + gap;
        // pause a moment at the start of each turn
        const float t = static_cast<float>(std::fmod(rainbow::Seconds() * speed, static_cast<double>(cycle + speed * 1.5f)));
        const float off = std::max(0.0f, t - speed * 1.5f);
        rh::Text(draw, m, ImVec2(pos.x - off, pos.y - m.capTop), text, color);
        rh::Text(draw, m, ImVec2(pos.x - off + cycle, pos.y - m.capTop), text, color);
    } else {
        rh::Text(draw, m, ImVec2(pos.x, pos.y - m.capTop), text, color);
    }
    draw->PopClipRect();
}

void Bar(ImDrawList* draw, ImVec2 min, float width, float height, float frac, ImVec4 color, float alpha) {
    const ImU32 track = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f * alpha));
    color.w *= alpha;
    draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), track);
    draw->AddRectFilled(min, ImVec2(min.x + width * std::clamp(frac, 0.0f, 1.0f), min.y + height),
                        ImGui::ColorConvertFloat4ToU32(color));
}

}  // namespace

NowPlaying::NowPlaying()
    : Module("now_playing", "Now playing",
             "Shows the song playing on your PC (Spotify, the browser, Apple Music...) and lets you pause, skip or "
             "seek from the radial menu, with the Halo around it.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.01f;
    l.y = 0.55f;
    SetDefaultLayout(l);
}

const char* NowPlaying::IconSvg() const {
    return "M9 18V6L19 4V16 M9 18C9 19.38 7.88 20.5 6.5 20.5C5.12 20.5 4 19.38 4 18C4 16.62 5.12 15.5 6.5 15.5C7.88 15.5 9 16.62 9 18Z M19 16C19 17.38 17.88 18.5 16.5 18.5C15.12 18.5 14 17.38 14 16C14 14.62 15.12 13.5 16.5 13.5C17.88 13.5 19 14.62 19 16Z";
}

void NowPlaying::OnEnable() {
    mu::Get().active = true;
    lyrics::SetHdCovers(mu::Get().hdCovers);
    lyrics::Want(lyrics::kCovers, mu::Get().hdCovers);
    media::Want(media::kHud, true);
    media::Want(media::kRadial, mu::Get().halo);
}

void NowPlaying::OnDisable() {
    lyrics::Want(lyrics::kHud, false);
    lyrics::Want(lyrics::kCovers, false);
    mu::Get().active = false;
    media::Want(media::kHud, false);
    media::Want(media::kRadial, false);
}

ImVec2 NowPlaying::OnRenderHud(const HudFrame& frame) {
    const mu::Settings& s = mu::Get();
    media::Track t = media::Current();
    const bool visible = t.present && t.title[0] != '\0' && (t.playing || !s.hideWhenPaused);
    if (!visible) {
        if (!frame.editing && (s.hideWhenNothing || !t.present)) {
            return ImVec2(0, 0);
        }
        // Something to place in the HUD editor.
        t = media::Track{};
        t.present = true;
        std::snprintf(t.title, sizeof(t.title), "%s", "Song title");
        std::snprintf(t.artist, sizeof(t.artist), "%s", "Artist");
        std::snprintf(t.app, sizeof(t.app), "%s", "Spotify");
        t.duration = 243.0;
        t.position = 97.0;
    }
    // A new song fades in.
    const double now = rainbow::Seconds();
    if (t.trackId != shownTrack_) {
        shownTrack_ = t.trackId;
        changedAt_ = now;
    }
    const float fade = s.animateChanges ? static_cast<float>(std::clamp((now - changedAt_) / 0.4, 0.0, 1.0)) : 1.0f;

    ImDrawList* draw = frame.draw;
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float pad = 5.0f * u;
    const float gap = 5.0f * u;
    const float lineH = m.capHeight + 4.0f * u;
    const ImU32 bright = WithAlpha(rh::TextColor(), fade);
    const ImU32 dim = WithAlpha(rh::TextColor(0.65f), fade);
    ImVec4 textCol = ImGui::ColorConvertU32ToFloat4(rh::TextColor());
    const ImVec4 accent = mu::AccentFor(s.accent, t, textCol);
    const float frac = t.duration > 0.0 ? static_cast<float>(t.position / t.duration) : 0.0f;
    const int style = std::clamp(s.hudStyle, 0, 6);
    lyrics::Want(lyrics::kHud, style == 5 || style == 6);



    // The texts.
    char second[360];
    if (s.showAlbum && t.album[0] != '\0') {
        std::snprintf(second, sizeof(second), "%s - %s", t.artist, t.album);
    } else {
        std::snprintf(second, sizeof(second), "%s", t.artist);
    }
    char time[48] = "";
    if (s.showTime && t.duration > 0.0) {
        char a[16], b[16];
        mu::FormatTime(t.position, a, sizeof(a));
        mu::FormatTime(t.duration, b, sizeof(b));
        std::snprintf(time, sizeof(time), "%s / %s", a, b);
    }
    const char* lines[4];
    ImU32 colors[4];
    int count = 0;
    lines[count] = t.title;
    colors[count++] = bright;
    if (s.showArtist && second[0] != '\0') {
        lines[count] = second;
        colors[count++] = dim;
    }
    if (s.showApp && t.app[0] != '\0') {
        lines[count] = t.app;
        colors[count++] = dim;
    }
    if (style != 1 && time[0] != '\0' && style != 3) {
        lines[count] = time;
        colors[count++] = dim;
    }

    // The lyric line being sung (and the next one), with REAL timings only:
    // word by word if the source has word times, otherwise the whole line.
    struct LyricNow {
        const char* line = nullptr;
        const char* next = nullptr;
        float sungWidth = -1.0f;  // >= 0: only this much of the line is bright (word timings)
    };
    auto lyricNow = [&](float maxW) {
        LyricNow out;
        const lyrics::Result& ly = mu::CurrentLyrics();
        if (ly.trackId != t.trackId || ly.status != lyrics::Status::Found || !ly.synced) return out;
        const int ms = mu::LyricsMs(t);
        const int i = lyrics::LineAt(ly, ms);
        const int n = static_cast<int>(ly.lines.size());
        if (i >= 0) {
            const lyrics::Line& line = ly.lines[static_cast<std::size_t>(i)];
            out.line = line.text.find_first_not_of(' ') == std::string::npos ? "..." : line.text.c_str();
            if (!line.words.empty()) {
                float w = 0.0f;
                for (const lyrics::Word& word : line.words) {
                    if (ms < word.ms) break;
                    w += rh::TextSize(m, word.text.c_str()).x;
                }
                out.sungWidth = std::min(w, maxW);
            }
        }
        if (s.lyricsNextLine && i + 1 < n) out.next = ly.lines[static_cast<std::size_t>(i + 1)].text.c_str();
        return out;
    };
    auto drawLyric = [&](const LyricNow& lyric, ImVec2 at, float w) {
        if (lyric.line == nullptr) return;
        if (lyric.sungWidth >= 0.0f) {
            Line(draw, m, at, w, lyric.line, dim, false);
            if (lyric.sungWidth > 0.0f) {
                draw->PushClipRect(ImVec2(at.x, at.y - m.capHeight), ImVec2(at.x + lyric.sungWidth, at.y + m.capHeight * 3.0f), true);
                Line(draw, m, at, w, lyric.line, bright, false);
                draw->PopClipRect();
            }
        } else {
            Line(draw, m, at, w, lyric.line, bright, false);
        }
        if (lyric.next != nullptr) {
            Line(draw, m, ImVec2(at.x, at.y + lineH), w, lyric.next, WithAlpha(dim, 0.7f), false);
        }
    };

    // --- Lyrics: the line being sung and the next one ---
    if (style == 5) {
        const float maxW = maxTextWidthForLyrics(s, u);
        LyricNow lyric = lyricNow(maxW);
        if (lyric.line == nullptr) lyric.line = t.title;  // no lyrics: the song's name
        const float w1 = rh::TextSize(m, lyric.line).x;
        const float w2 = lyric.next != nullptr ? rh::TextSize(m, lyric.next).x : 0.0f;
        const float w = std::min(maxW, std::max({w1, w2, 40.0f * u}));
        const float width = std::floor(pad * 2.0f + w);
        const float height = std::floor(pad * 2.0f + m.capHeight + (lyric.next != nullptr ? lineH : 0.0f));
        const ImVec2 o = frame.pos;
        rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));
        drawLyric(lyric, ImVec2(o.x + pad, o.y + pad), w);
        return ImVec2(width, height);
    }

    // --- Wide + lyrics: art, song and progress on top, the lyrics under them ---
    if (style == 6) {
        const float maxW = maxTextWidthForLyrics(s, u);
        const LyricNow lyric = lyricNow(maxW);
        const float artW = s.showArt ? lineH * 3.0f - 4.0f * u : 0.0f;
        float infoW = std::max(rh::TextSize(m, t.title).x, rh::TextSize(m, second).x);
        infoW = std::clamp(infoW, 60.0f * u, maxW - artW - gap);
        float lyricW = 0.0f;
        if (lyric.line != nullptr) {
            lyricW = std::max(rh::TextSize(m, lyric.line).x, lyric.next != nullptr ? rh::TextSize(m, lyric.next).x : 0.0f);
        }
        const float innerW = std::min(maxW, std::max(artW + (artW > 0 ? gap : 0.0f) + infoW, lyricW));
        const float topH = std::max(artW, lineH * 3.0f - 4.0f * u);
        const float lyricH = lyric.line != nullptr ? gap + m.capHeight + (lyric.next != nullptr ? lineH : 0.0f) : 0.0f;
        const float width = std::floor(pad * 2.0f + innerW);
        const float height = std::floor(pad * 2.0f + topH + lyricH);
        const ImVec2 o = frame.pos;
        rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));
        float x = o.x + pad;
        if (artW > 0.0f) {
            const float spin = s.artShape == 2 && s.spinArt ? mu::SpinAngle() : 0.0f;
            mu::DrawArt(draw, ImVec2(x, o.y + pad), ImVec2(x + artW, o.y + pad + artW), s.artShape, spin, fade);
            x += artW + gap;
        }
        const float textW = o.x + pad + innerW - x;
        float y = o.y + pad;
        Line(draw, m, ImVec2(x, y), textW, t.title, bright, s.marquee);
        y += lineH;
        Line(draw, m, ImVec2(x, y), textW, second, dim, s.marquee);
        y += lineH;
        if (t.duration > 0.0) {
            float barW = textW;
            if (time[0] != '\0') {
                const float tw = rh::TextSize(m, time).x;
                barW = std::max(10.0f * u, textW - tw - gap);
                rh::Text(draw, m, ImVec2(x + barW + gap, y - m.capTop), time, dim);
            }
            Bar(draw, ImVec2(x, y + m.capHeight * 0.5f - u), barW, 2.0f * u, frac, accent, fade);
        }
        if (lyric.line != nullptr) {
            const float ly = o.y + pad + topH + gap;
            draw->AddLine(ImVec2(o.x + pad, ly - gap * 0.5f), ImVec2(o.x + pad + innerW, ly - gap * 0.5f),
                          WithAlpha(dim, 0.25f), std::max(1.0f, u * 0.5f));
            drawLyric(lyric, ImVec2(o.x + pad, ly), innerW);
        }
        return ImVec2(width, height);
    }

    const float maxText = static_cast<float>(s.maxWidth) * u;
    const ImVec2 o = frame.pos;

    // --- One line: "Title - Artist" ---
    if (style == 3) {
        char one[400];
        if (s.showArtist && t.artist[0] != '\0') {
            std::snprintf(one, sizeof(one), "%s - %s", t.title, t.artist);
        } else {
            std::snprintf(one, sizeof(one), "%s", t.title);
        }
        const float w = std::min(maxText, rh::TextSize(m, one).x);
        const float artW = s.showArt ? lineH : 0.0f;
        const float width = std::floor(pad * 2.0f + artW + (artW > 0 ? gap * 0.6f : 0.0f) + w);
        const float height = std::floor(pad * 2.0f + m.capHeight);
        rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));
        float x = o.x + pad;
        if (artW > 0.0f) {
            const float a = m.capHeight + 2.0f * u;
            mu::DrawArt(draw, ImVec2(x, o.y + (height - a) * 0.5f), ImVec2(x + a, o.y + (height + a) * 0.5f),
                        s.artShape, s.artShape == 2 && s.spinArt ? mu::SpinAngle() : 0.0f, fade);
            x += artW + gap * 0.6f;
        }
        Line(draw, m, ImVec2(x, o.y + pad), w, one, bright, s.marquee);
        return ImVec2(width, height);
    }

    // Width of the text column.
    float textW = 0.0f;
    for (int i = 0; i < count; ++i) {
        textW = std::max(textW, rh::TextSize(m, lines[i]).x);
    }
    textW = std::min(maxText, std::max(textW, 40.0f * u));
    const float textH = lineH * static_cast<float>(count) - 4.0f * u;

    // --- Card: big art on top, text under it ---
    if (style == 2) {
        const float artW = s.showArt ? std::max(textW, 70.0f * u) : 0.0f;
        const float colW = std::max(textW, artW);
        const float width = std::floor(pad * 2.0f + colW);
        float y = o.y + pad;
        const float barH = t.duration > 0.0 ? 2.0f * u + gap : 0.0f;
        const float height = std::floor(pad * 2.0f + (artW > 0 ? artW + gap : 0.0f) + textH + barH);
        rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));
        if (artW > 0.0f) {
            const float ax = o.x + pad + (colW - artW) * 0.5f;
            mu::DrawArt(draw, ImVec2(ax, y), ImVec2(ax + artW, y + artW), s.artShape,
                        s.artShape == 2 && s.spinArt ? mu::SpinAngle() : 0.0f, fade);
            y += artW + gap;
        }
        for (int i = 0; i < count; ++i) {
            Line(draw, m, ImVec2(o.x + pad, y), colW, lines[i], colors[i], s.marquee, 1.0f);
            y += lineH;
        }
        if (barH > 0.0f) {
            Bar(draw, ImVec2(o.x + pad, y - 4.0f * u + gap), colW, 2.0f * u, frac, accent, fade);
        }
        return ImVec2(width, height);
    }

    // --- Compact / with progress / vinyl: art on the left, text on the right ---
    const bool withBar = (style == 1 || style == 4) && t.duration > 0.0;
    const float barRow = withBar ? lineH : 0.0f;
    const float contentH = textH + barRow;
    float artW = 0.0f;
    if (s.showArt) {
        artW = style == 4 ? std::max(contentH, lineH * 3.0f) : std::max(contentH, lineH * 2.0f - 4.0f * u);
    }
    const float innerH = std::max(contentH, artW);
    const float width = std::floor(pad * 2.0f + artW + (artW > 0 ? gap : 0.0f) + textW);
    const float height = std::floor(pad * 2.0f + innerH);
    rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));
    float x = o.x + pad;
    if (artW > 0.0f) {
        const int shape = style == 4 ? 2 : s.artShape;
        const float spin = shape == 2 && (s.spinArt || style == 4) ? mu::SpinAngle() : 0.0f;
        mu::DrawArt(draw, ImVec2(x, o.y + pad), ImVec2(x + artW, o.y + pad + artW), shape, spin, fade);
        x += artW + gap;
    }
    float y = o.y + pad + (innerH - contentH) * 0.5f;
    for (int i = 0; i < count; ++i) {
        Line(draw, m, ImVec2(x, y), textW, lines[i], colors[i], s.marquee);
        y += lineH;
    }
    if (withBar) {
        // the bar, with the time at its right
        float barW = textW;
        if (time[0] != '\0') {
            const float tw = rh::TextSize(m, time).x;
            barW = std::max(10.0f * u, textW - tw - gap);
            rh::Text(draw, m, ImVec2(x + barW + gap, y - m.capTop), time, dim);
        }
        Bar(draw, ImVec2(x, y + m.capHeight * 0.5f - u), barW, 2.0f * u, frac, accent, fade);
    }
    return ImVec2(width, height);
}

bool NowPlaying::DrawSettings() {
    bool changed = mu::DrawStyleSettings();
    changed |= mu::DrawHaloSettings();
    changed |= mu::DrawLyricsSettings();
    changed |= mu::DrawAlbumColorSettings();
    ui::Hint("Works with any app that shows its music in Windows' volume panel: Spotify, browsers (YouTube, "
             "SoundCloud), Apple Music and more. Some apps don't share the position, so the bar may jump.");
    ui::Hint("Tip: the Music page of the menu shows all of this bigger, with the lyrics.");
    return changed;
}

void NowPlaying::SaveSettings(nlohmann::json& out) const {
    const mu::Settings& s = mu::Get();
    out["hudStyle"] = s.hudStyle;
    out["artShape"] = s.artShape;
    out["spinArt"] = s.spinArt;
    out["showArt"] = s.showArt;
    out["showArtist"] = s.showArtist;
    out["showAlbum"] = s.showAlbum;
    out["showApp"] = s.showApp;
    out["showTime"] = s.showTime;
    out["accent"] = s.accent;
    out["marquee"] = s.marquee;
    out["maxWidth"] = s.maxWidth;
    out["hideWhenPaused"] = s.hideWhenPaused;
    out["hideWhenNothing"] = s.hideWhenNothing;
    out["animateChanges"] = s.animateChanges;
    out["halo"] = s.halo;
    out["haloThickness"] = s.haloThickness;
    out["haloGap"] = s.haloGap;
    out["haloColor"] = s.haloColor;
    out["haloTrack"] = s.haloTrack;
    out["haloKnob"] = s.haloKnob;
    out["haloArtCenter"] = s.haloArtCenter;
    out["haloArtShape"] = s.haloArtShape;
    out["haloTitle"] = s.haloTitle;
    out["haloButtons"] = s.haloButtons;
    out["lyricsOnline"] = s.lyricsOnline;
    out["lyricsOffset"] = s.lyricsOffset;
    out["lyricsNextLine"] = s.lyricsNextLine;
    out["hudFollowAlbum"] = s.hudFollowAlbum;
    out["hdCovers"] = s.hdCovers;
    out["menuFollowAlbum"] = s.menuFollowAlbum;
}

void NowPlaying::LoadSettings(const nlohmann::json& in) {
    mu::Settings& s = mu::Get();
    const mu::Settings d;
    s.hudStyle = std::clamp(in.value("hudStyle", d.hudStyle), 0, 6);
    s.artShape = std::clamp(in.value("artShape", d.artShape), 0, 2);
    s.spinArt = in.value("spinArt", d.spinArt);
    s.showArt = in.value("showArt", d.showArt);
    s.showArtist = in.value("showArtist", d.showArtist);
    s.showAlbum = in.value("showAlbum", d.showAlbum);
    s.showApp = in.value("showApp", d.showApp);
    s.showTime = in.value("showTime", d.showTime);
    s.accent = std::clamp(in.value("accent", d.accent), 0, 2);
    s.marquee = in.value("marquee", d.marquee);
    s.maxWidth = std::clamp(in.value("maxWidth", d.maxWidth), 60, 320);
    s.hideWhenPaused = in.value("hideWhenPaused", d.hideWhenPaused);
    s.hideWhenNothing = in.value("hideWhenNothing", d.hideWhenNothing);
    s.animateChanges = in.value("animateChanges", d.animateChanges);
    s.halo = in.value("halo", d.halo);
    s.haloThickness = std::clamp(in.value("haloThickness", d.haloThickness), 0.1f, 1.0f);
    s.haloGap = std::clamp(in.value("haloGap", d.haloGap), 0.5f, 4.0f);
    s.haloColor = std::clamp(in.value("haloColor", d.haloColor), 0, 2);
    s.haloTrack = in.value("haloTrack", d.haloTrack);
    s.haloKnob = in.value("haloKnob", d.haloKnob);
    s.haloArtCenter = in.value("haloArtCenter", d.haloArtCenter);
    s.haloArtShape = std::clamp(in.value("haloArtShape", d.haloArtShape), 0, 2);
    s.haloTitle = in.value("haloTitle", d.haloTitle);
    s.haloButtons = std::clamp(in.value("haloButtons", d.haloButtons), 0, 2);
    s.lyricsOnline = in.value("lyricsOnline", d.lyricsOnline);
    s.lyricsOffset = std::clamp(in.value("lyricsOffset", d.lyricsOffset), -5.0f, 5.0f);
    s.lyricsNextLine = in.value("lyricsNextLine", d.lyricsNextLine);
    s.hudFollowAlbum = in.value("hudFollowAlbum", d.hudFollowAlbum);
    s.hdCovers = in.value("hdCovers", d.hdCovers);
    s.menuFollowAlbum = in.value("menuFollowAlbum", d.menuFollowAlbum);
    lyrics::SetOnline(s.lyricsOnline);
    if (IsEnabled()) {
        OnEnable();
    }
}

}  // namespace luma::modules::hud
