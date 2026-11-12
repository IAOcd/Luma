// =============================================================================
//  lyric_flow.cpp - Lyric Flow: the lyrics written IN the world (see lyric_flow.h)
// =============================================================================
//  The letters are real geometry of the world, not a picture on top:
//    1. Each word is "pixelated" with the game's own pixel font: every lit
//       pixel of every letter becomes a small square (consecutive lit
//       pixels of a row are merged into one longer rectangle, so there are
//       few of them). Its border (outline) is computed the same way. This is
//       done ONCE, when the line starts.
//    2. Every frame, those squares are placed on a "sign" in the sky (its
//       place in the world, its size, its tilt, the effects) and handed to
//       sdk/draw3d as LINES: a few thin lines per square, enough to look
//       solid at that distance. (Filled triangles don't show far away with
//       the game's materials we can use; its outline material, the one of
//       the hitboxes, always does.)
//    3. draw3d gives them to the game together with the rest of the world:
//       the game draws them with depth, so a mountain or a cloud in front
//       covers them.
//
//  TWO PLACES:
//    - In the world: the sign is at a fixed place. Walk to it and it gets
//      bigger; walk away and it gets smaller.
//    - In the sky: the sign is at a DIRECTION, very far away, and it moves
//      with you (like the sun or the skybox): you never get closer, but when
//      you turn, it stays where it was in the sky.
// =============================================================================
#include "render/lyric_flow.h"

#include <windows.h>  // GDI: Windows draws each letter of a font for us (GetGlyphOutline)

#include <imgui_internal.h>  // ImTextCharFromUtf8 (reads one letter of a UTF-8 text)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/lyrics.h"
#include "core/media.h"
#include "render/hud.h"
#include "render/music.h"
#include "render/rainbow.h"
#include "render/theme.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"

namespace luma::render::lyricflow {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kPixelCells = 11.0f;     // Minecraft's pixel font: a line of text is 11 "pixels" tall
constexpr float kSkyDistance = 140.0f;   // "in the sky": this far away (blocks)

enum Appear { kPop, kFade, kDrop, kRise, kZoom, kSpin, kFlip, kTypewriter, kScatter };
enum Leave { kFloatUp, kFadeOut, kFall, kShrink, kScatterOut };
enum Motion { kNone, kFloat, kWave, kSway, kPulse };

// A lit piece of a word: a rectangle in font pixels (x right, y down).
struct Rect {
    float x0, y0, x1, y1;
    int letter;  // which letter of the word it belongs to (for the typewriter)
};

struct Word {
    int ms = 0;
    int duration = 300;   // until the next word (ms)
    float x = 0, y = 0;   // its center on the sign, in blocks (right, up)
    float size = 1.0f;    // its height, in blocks
    float tilt = 0.0f;    // radians
    float phase = 0.0f;   // for the motions (each word a little different)
    float width = 0.0f;   // in font pixels
    float cells = kPixelCells;  // how many font pixels tall a line is (its height in "pixels")
    int letters = 1;
    std::vector<Rect> rects;
    std::vector<Rect> outline;
};

struct FlowLine {
    int index = -1;
    std::uint32_t track = 0;
    float offset[3]{};  // the sign's center, relative to the camera when it was written
    float anchor[3]{};  // ... and in the world ("in the world" mode)
    float right[3]{};   // the sign's horizontal direction (it faces you when it was written)
    std::vector<Word> words;
    int leaveStart = 0;  // the song's time when it starts leaving...
    int leaveEnd = 0;    // ...and when it's gone (= when the next line starts)
};

FlowLine g_current;
FlowLine g_previous;
int g_lineCount = 0;
bool g_sent = false;  // did we hand shapes to draw3d (to clear them once when stopping)

float Hash01(std::uint32_t a, std::uint32_t b) {
    std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0xC2B2AE3Du;
    h ^= h >> 13;
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

std::string Upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// Rows of lit cells -> rectangles (consecutive cells of a row = one rectangle).
void Runs(const std::vector<std::uint8_t>& grid, int w, int h, float ox, float oy, int letter, std::vector<Rect>& out) {
    for (int r = 0; r < h; ++r) {
        int start = -1;
        for (int c = 0; c <= w; ++c) {
            const bool lit = c < w && grid[static_cast<std::size_t>(r) * w + c] != 0;
            if (lit && start < 0) start = c;
            if (!lit && start >= 0) {
                out.push_back(Rect{ox + static_cast<float>(start), oy + static_cast<float>(r), ox + static_cast<float>(c),
                                   oy + static_cast<float>(r + 1), letter});
                start = -1;
            }
        }
    }
}

// "Pixelates" a word: reads its letters from the font's picture (the ImGui
// font atlas, in memory) on a grid of 11 rows, keeps the lit cells, and
// computes the outline: the empty cells that touch a lit one.
// The outline: the empty cells (in a grid one cell bigger on every side)
// that touch a lit one.
void Outline(const std::vector<std::uint8_t>& grid, int cw, int ch, float gx, float gy, int letter, std::vector<Rect>& out) {
    const int rw = cw + 2, rh = ch + 2;
    std::vector<std::uint8_t> ring(static_cast<std::size_t>(rw) * rh, 0);
    for (int r = 0; r < rh; ++r) {
        for (int col = 0; col < rw; ++col) {
            const int gr = r - 1, gc = col - 1;
            const bool self = gr >= 0 && gr < ch && gc >= 0 && gc < cw && grid[static_cast<std::size_t>(gr) * cw + gc];
            if (self) continue;
            bool touches = false;
            for (int dr = -1; dr <= 1 && !touches; ++dr) {
                for (int dc = -1; dc <= 1 && !touches; ++dc) {
                    const int nr = gr + dr, nc = gc + dc;
                    touches = nr >= 0 && nr < ch && nc >= 0 && nc < cw && grid[static_cast<std::size_t>(nr) * cw + nc];
                }
            }
            ring[static_cast<std::size_t>(r) * rw + col] = touches;
        }
    }
    Runs(ring, rw, rh, gx - 1.0f, gy - 1.0f, letter, out);
}

// --- Windows fonts (GDI) --------------------------------------------------------------
// Windows can draw any letter of any installed font as a small grayscale
// picture (GetGlyphOutline, GGO_GRAY8_BITMAP). We ask for letters "cells"
// pixels tall and keep the cells that are more than ~half lit. The font is
// created once and kept while the settings don't change.
struct GdiFont {
    HDC dc = nullptr;
    HFONT font = nullptr;
    int face = -1, cells = 0;
    bool bold = false;
    int ascent = 0;
};
GdiFont g_gdi;

bool EnsureGdiFont(int face, int cells, bool bold) {
    if (g_gdi.font != nullptr && g_gdi.face == face && g_gdi.cells == cells && g_gdi.bold == bold) return true;
    if (g_gdi.dc == nullptr) g_gdi.dc = CreateCompatibleDC(nullptr);
    if (g_gdi.dc == nullptr) return false;
    if (g_gdi.font != nullptr) {
        DeleteObject(g_gdi.font);
        g_gdi.font = nullptr;
    }
    wchar_t name[LF_FACESIZE]{};
    MultiByteToWideChar(CP_UTF8, 0, kFonts[face], -1, name, LF_FACESIZE);
    // Positive height = the whole line (ascent + descent) is "cells" pixels.
    g_gdi.font = CreateFontW(cells, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, name);
    if (g_gdi.font == nullptr) return false;
    SelectObject(g_gdi.dc, g_gdi.font);
    TEXTMETRICW tm{};
    GetTextMetricsW(g_gdi.dc, &tm);
    g_gdi.ascent = tm.tmAscent;
    g_gdi.face = face;
    g_gdi.cells = cells;
    g_gdi.bold = bold;
    return true;
}

void ReleaseGdiFont() {
    if (g_gdi.font != nullptr) DeleteObject(g_gdi.font);
    if (g_gdi.dc != nullptr) DeleteDC(g_gdi.dc);
    g_gdi = GdiFont{};
}

bool PixelateGdi(Word& word, const std::string& text, const music::Settings& s) {
    const int cells = std::clamp(s.flowResolution, 9, 32);
    if (!EnsureGdiFont(std::clamp(s.flowFont, 1, kFontCount - 1), cells, s.flowBold)) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(0, n)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
    const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    std::vector<std::uint8_t> bitmap, grid;
    float cursor = 0.0f;
    for (wchar_t c : wide) {
        if (c == 0) break;
        GLYPHMETRICS gm{};
        const DWORD size = GetGlyphOutlineW(g_gdi.dc, c, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &identity);
        if (size != GDI_ERROR && size > 0 && gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
            bitmap.assign(size, 0);
            GetGlyphOutlineW(g_gdi.dc, c, GGO_GRAY8_BITMAP, &gm, size, bitmap.data(), &identity);
            const int cw = static_cast<int>(gm.gmBlackBoxX), ch = static_cast<int>(gm.gmBlackBoxY);
            const int pitch = (cw + 3) & ~3;  // each row is padded to 4 bytes
            grid.assign(static_cast<std::size_t>(cw) * ch, 0);
            for (int r = 0; r < ch; ++r) {
                for (int col = 0; col < cw; ++col) {
                    grid[static_cast<std::size_t>(r) * cw + col] = bitmap[static_cast<std::size_t>(r) * pitch + col] > 26;  // (0..64)
                }
            }
            const float gx = std::round(cursor + static_cast<float>(gm.gmptGlyphOrigin.x));
            const float gy = static_cast<float>(g_gdi.ascent - gm.gmptGlyphOrigin.y);
            Runs(grid, cw, ch, gx, gy, word.letters, word.rects);
            Outline(grid, cw, ch, gx, gy, word.letters, word.outline);
            ++word.letters;
        }
        cursor += static_cast<float>(gm.gmCellIncX) + s.flowLetterSpacing;
    }
    word.width = std::max(1.0f, cursor - s.flowLetterSpacing);
    word.cells = static_cast<float>(cells);
    word.letters = std::max(1, word.letters);
    return !word.rects.empty();
}

void Pixelate(Word& word, const std::string& text, const music::Settings& settings) {
    word.rects.clear();
    word.outline.clear();
    word.width = 0.0f;
    word.letters = 0;
    word.cells = kPixelCells;
    if (settings.flowFont != 0 && PixelateGdi(word, text, settings)) return;
    word.rects.clear();
    word.outline.clear();
    word.letters = 0;
    word.cells = kPixelCells;
    ImFont* font = hud::PixelFont();
    if (font == nullptr) font = theme::GetFonts().title != nullptr ? theme::GetFonts().title : ImGui::GetFont();
    unsigned char* pixels = nullptr;
    int tw = 0, th = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
    if (pixels == nullptr || tw <= 0 || th <= 0) return;
    const float scale = kPixelCells / font->FontSize;  // font units -> cells
    float cursor = 0.0f;                          // in font units
    const char* s = text.c_str();
    const char* end = s + text.size();
    std::vector<std::uint8_t> grid;
    while (s < end) {
        unsigned int c = 0;
        s += ImTextCharFromUtf8(&c, s, end);
        if (c == 0) break;
        const ImFontGlyph* g = font->FindGlyph(static_cast<ImWchar>(c));
        if (g == nullptr) continue;
        if (g->Visible) {
            const int cw = std::max(1, static_cast<int>(std::lround((g->X1 - g->X0) * scale)));
            const int ch = std::max(1, static_cast<int>(std::lround((g->Y1 - g->Y0) * scale)));
            const float gx = std::round((cursor + g->X0) * scale);
            const float gy = std::round(g->Y0 * scale);
            grid.assign(static_cast<std::size_t>(cw) * ch, 0);
            for (int r = 0; r < ch; ++r) {
                for (int col = 0; col < cw; ++col) {
                    const float u = g->U0 + (g->U1 - g->U0) * (static_cast<float>(col) + 0.5f) / static_cast<float>(cw);
                    const float v = g->V0 + (g->V1 - g->V0) * (static_cast<float>(r) + 0.5f) / static_cast<float>(ch);
                    const int px = std::clamp(static_cast<int>(u * static_cast<float>(tw)), 0, tw - 1);
                    const int py = std::clamp(static_cast<int>(v * static_cast<float>(th)), 0, th - 1);
                    grid[static_cast<std::size_t>(r) * cw + col] = pixels[(static_cast<std::size_t>(py) * tw + px) * 4 + 3] > 110;
                }
            }
            Runs(grid, cw, ch, gx, gy, word.letters, word.rects);
            Outline(grid, cw, ch, gx, gy, word.letters, word.outline);
            ++word.letters;
        }
        cursor += g->AdvanceX + settings.flowLetterSpacing / scale;
    }
    word.width = std::max(1.0f, cursor * scale - settings.flowLetterSpacing);
    word.letters = std::max(1, word.letters);
}

// The view's position and horizontal direction.
bool FlatView(float pos[3], float flat[3]) {
    sdk::camera::View v;
    if (!sdk::camera::GetView(v)) return false;
    std::memcpy(pos, v.pos, sizeof(v.pos));
    const float yaw = v.yaw * kDegToRad;
    flat[0] = -std::sin(yaw);  // the game's yaw: 0 = south (+Z), 90 = west (-X)
    flat[1] = 0.0f;
    flat[2] = std::cos(yaw);
    return true;
}

void BuildLine(FlowLine& f, const lyrics::Result& ly, int index, std::uint32_t track, const music::Settings& s) {
    f = FlowLine{};
    f.index = index;
    f.track = track;
    float pos[3], flat[3];
    if (!FlatView(pos, flat)) return;
    const lyrics::Line& line = ly.lines[static_cast<std::size_t>(index)];
    f.right[0] = -flat[2];
    f.right[1] = 0.0f;
    f.right[2] = flat[0];
    const float side = (g_lineCount++ % 2 == 0 ? -1.0f : 1.0f) * s.flowDistance * 0.12f;
    for (int k = 0; k < 3; ++k) f.offset[k] = flat[k] * s.flowDistance + f.right[k] * side;
    f.offset[1] = s.flowHeight;
    for (int k = 0; k < 3; ++k) f.anchor[k] = pos[k] + f.offset[k];

    // The words (syllables joined), or the line split by spaces.
    struct Piece {
        std::string text;
        int ms;
    };
    std::vector<Piece> pieces;
    if (!line.words.empty()) {
        std::string word;
        int start = 0;
        for (std::size_t i = 0; i < line.words.size(); ++i) {
            if (word.empty()) start = line.words[i].ms;
            word += line.words[i].text;
            if (!word.empty() && (word.back() == ' ' || i + 1 == line.words.size())) {
                while (!word.empty() && word.back() == ' ') word.pop_back();
                if (!word.empty()) pieces.push_back(Piece{word, start});
                word.clear();
            }
        }
    } else {
        std::size_t p = 0;
        int i = 0;
        while (p < line.text.size()) {
            const std::size_t e = line.text.find(' ', p);
            const std::string w = line.text.substr(p, e == std::string::npos ? std::string::npos : e - p);
            if (!w.empty()) pieces.push_back(Piece{w, line.ms + 90 * i++});
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    const int lineEnd = index + 1 < static_cast<int>(ly.lines.size()) ? ly.lines[static_cast<std::size_t>(index + 1)].ms
                                                                       : line.ms + 4000;

    // Lay them out in rows, each word with its own size, height and tilt.
    const float base = s.flowSize;
    const float maxRow = base * 13.0f;
    struct Row {
        float width = 0.0f;
        std::size_t first = 0, count = 0;
    };
    std::vector<Row> rows(1);
    float x = 0.0f;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        Word w;
        w.ms = pieces[i].ms;
        const int next = i + 1 < pieces.size() ? pieces[i + 1].ms : lineEnd;
        w.duration = std::clamp(next - w.ms, 120, 1500);
        Pixelate(w, s.flowUppercase ? Upper(pieces[i].text) : pieces[i].text, s);
        if (w.rects.empty()) continue;
        const float r1 = Hash01(static_cast<std::uint32_t>(index), static_cast<std::uint32_t>(i));
        const float r2 = Hash01(static_cast<std::uint32_t>(index) + 977u, static_cast<std::uint32_t>(i));
        const float r3 = Hash01(static_cast<std::uint32_t>(index) + 4211u, static_cast<std::uint32_t>(i));
        w.size = base * (0.8f + 0.55f * r1 * r1);
        w.tilt = (r2 - 0.5f) * 2.0f * s.flowTilt * kDegToRad;
        w.phase = r3 * 6.2831f;
        const float widthBlocks = w.width / w.cells * w.size;
        const float gap = base * s.flowWordSpacing;
        if (x > 0.0f && x + widthBlocks > maxRow) {
            rows.push_back(Row{});
            rows.back().first = f.words.size();
            x = 0.0f;
        }
        w.x = x + widthBlocks * 0.5f;
        w.y = -static_cast<float>(rows.size() - 1) * base * 1.35f + (r3 - 0.5f) * base * 0.45f;
        x += widthBlocks + gap;
        rows.back().width = x - gap;
        ++rows.back().count;
        f.words.push_back(std::move(w));
    }
    const float totalH = static_cast<float>(rows.size() - 1) * base * 1.35f;
    for (const Row& r : rows) {
        for (std::size_t i = r.first; i < r.first + r.count && i < f.words.size(); ++i) {
            f.words[i].x -= r.width * 0.5f;
            f.words[i].y += totalH * 0.5f;
        }
    }
    // WHEN IT LEAVES: it must be gone when the next line starts (so they never
    // overlap), so it starts leaving "leave time" before that. But never
    // before its last word was sung, and if a long instrumental comes, it
    // doesn't wait there forever: at most 2.5 s after its last word.
    const int leaveMs = static_cast<int>(s.flowLeaveTime * 1000.0f / std::max(0.3f, s.flowSpeed));
    int lastWord = line.ms, lastWordEnd = line.ms + 300;
    if (!f.words.empty()) {
        lastWord = f.words.back().ms;
        lastWordEnd = f.words.back().ms + f.words.back().duration;
    }
    f.leaveEnd = std::min(lineEnd, lastWordEnd + 2500 + leaveMs);
    f.leaveStart = std::max(lastWord + 150, f.leaveEnd - leaveMs);
    if (f.leaveStart > f.leaveEnd - 100) f.leaveStart = f.leaveEnd - 100;  // a very short gap: a quick exit
}

// --- Easing ---------------------------------------------------------------------------------
float Clamp01(float t) {
    return std::clamp(t, 0.0f, 1.0f);
}
float EaseOut(float t) {
    t = Clamp01(t);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
// easeOutBack: arrives a little past 1 and settles (the "pop").
float Back(float t) {
    t = Clamp01(t);
    constexpr float c1 = 1.9f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// --- Colors ----------------------------------------------------------------------------------
void WordColor(const music::Settings& s, std::size_t i, std::size_t count, double now, float out[3]) {
    const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
    float album[3] = {s.flowColor[0], s.flowColor[1], s.flowColor[2]};
    music::AlbumColor(album);
    switch (s.flowColorMode) {
        case 1:  // the album's color
            std::memcpy(out, album, sizeof(album));
            return;
        case 2:  // from the album's color to a light version of it
            for (int k = 0; k < 3; ++k) {
                const float light = album[k] + (1.0f - album[k]) * 0.65f;
                out[k] = album[k] + (light - album[k]) * t;
            }
            return;
        case 3:  // two colors, across the line
            for (int k = 0; k < 3; ++k) out[k] = s.flowColor[k] + (s.flowColor2[k] - s.flowColor[k]) * t;
            return;
        case 4: {  // rainbow: a hue per word, slowly turning
            const double turns = now * 0.12 + static_cast<double>(t) * 0.8;
            rainbow::Hsv(static_cast<float>(turns - std::floor(turns)), 0.55f, 1.0f, out[0], out[1], out[2]);
            return;
        }
        default:
            std::memcpy(out, s.flowColor, sizeof(float) * 3);
            return;
    }
}

struct ViewInfo {
    bool ok = false;
    float pos[3]{};
    float forward[3]{0.0f, 0.0f, 1.0f};  // where the camera looks
    float scaleY = 1.0f;
};

// Where the sign is NOW, and how much bigger it is (the sky mode keeps it far).
void SignPlace(const FlowLine& f, const ViewInfo& view, const music::Settings& s, float anchor[3], float& grow) {
    grow = 1.0f;
    if (s.flowMode == 1 && view.ok) {
        // In the sky: the same direction as when it was written, very far, around the camera.
        const float len = std::max(0.001f, std::sqrt(f.offset[0] * f.offset[0] + f.offset[1] * f.offset[1] + f.offset[2] * f.offset[2]));
        grow = kSkyDistance / len;
        for (int k = 0; k < 3; ++k) anchor[k] = view.pos[k] + f.offset[k] * grow;
        return;
    }
    std::memcpy(anchor, f.anchor, sizeof(f.anchor));
}

// Turns the line's visible words into lines in the world.
void AddLine(std::vector<sdk::draw3d::WorldShapes>& out, const FlowLine& f, int ms, float leave, const music::Settings& s,
             bool animate, const ViewInfo& view, float displayH, double now) {
    float anchor[3];
    float grow = 1.0f;
    SignPlace(f, view, s, anchor, grow);
    const float speed = std::max(0.3f, s.flowSpeed);
    // Leaving (leave goes 1 -> 0). "gone" is eased: it starts slowly and
    // speeds up, like something that lets go (not a robotic slide).
    const float raw = 1.0f - leave;
    const float gone = raw * raw * (3.0f - 2.0f * raw);
    float leaveDy = 0.0f, leaveScale = 1.0f;
    if (s.flowLeave == kFloatUp) leaveDy = gone * s.flowSize * 2.0f;
    if (s.flowLeave == kFall) leaveDy = -raw * raw * s.flowSize * 5.0f;
    if (s.flowLeave == kShrink) leaveScale = 1.0f - gone;
    // The word being sung (the last one that started).
    int singing = -1;
    for (std::size_t i = 0; i < f.words.size(); ++i) {
        if (ms >= f.words[i].ms) singing = static_cast<int>(i);
    }
    const float motionT = static_cast<float>(now) * speed;

    for (std::size_t i = 0; i < f.words.size(); ++i) {
        const Word& w = f.words[i];
        const float age = static_cast<float>(ms - w.ms) * speed;
        if (age < 0.0f) continue;  // not sung yet: not written yet
        // --- Appearing ---
        const float appear = animate ? EaseOut(age / 320.0f) : 1.0f;
        float alpha = animate ? Clamp01(age / 160.0f) : 1.0f;
        float scaleX = 1.0f, scaleY = 1.0f, dy = 0.0f, spin = 0.0f, scatter = 0.0f;
        int lettersShown = w.letters;
        if (animate) {
            switch (s.flowAppear) {
                case kPop: scaleX = scaleY = 0.45f + 0.55f * Back(age / 320.0f); break;
                case kFade: alpha = Clamp01(age / 450.0f); break;
                case kDrop: dy = (1.0f - Back(age / 420.0f)) * w.size * 2.5f; break;
                case kRise: dy = -(1.0f - Back(age / 420.0f)) * w.size * 2.5f; break;
                case kZoom:
                    scaleX = scaleY = 1.0f + (1.0f - appear) * 2.0f;
                    alpha = Clamp01(age / 300.0f);
                    break;
                case kSpin:
                    spin = (1.0f - appear) * kPi;
                    scaleX = scaleY = 0.5f + 0.5f * appear;
                    break;
                case kFlip: scaleX = std::max(0.02f, Back(age / 380.0f)); break;
                case kTypewriter:
                    lettersShown = std::min(
                        w.letters, 1 + static_cast<int>(age / (static_cast<float>(w.duration) * 0.8f) * static_cast<float>(w.letters)));
                    alpha = 1.0f;
                    break;
                case kScatter:
                    scatter = 1.0f - EaseOut(age / 520.0f);
                    alpha = Clamp01(age / 200.0f);
                    break;
                default: break;
            }
        }
        if (s.flowLeave == kScatterOut) scatter = std::max(scatter, gone * 0.8f);
        // --- Moving while it's there ---
        float motionY = 0.0f, motionTilt = 0.0f, pulse = 1.0f;
        if (animate) {
            switch (s.flowMotion) {
                case kFloat: motionY = std::sin(motionT * 1.6f + w.phase) * w.size * 0.10f; break;
                case kWave: motionY = std::sin(motionT * 3.0f - w.x * 0.5f) * w.size * 0.16f; break;
                case kSway: motionTilt = std::sin(motionT * 1.3f + w.phase) * 6.0f * kDegToRad; break;
                case kPulse:
                    if (static_cast<int>(i) == singing) {
                        const float k = Clamp01(static_cast<float>(ms - w.ms) / static_cast<float>(w.duration));
                        pulse = 1.0f + 0.14f * std::sin(k * kPi);
                    }
                    break;
                default: break;
            }
        }
        // It fades a bit ahead of the movement (gone at 80% of the way), so
        // the end of an exit is never a half-visible smear.
        const float fade = std::clamp(1.0f - raw / 0.8f, 0.0f, 1.0f);
        alpha *= (s.flowLeave == kShrink ? std::min(1.0f, fade * 2.0f) : fade) * s.flowOpacity;
        if (alpha <= 0.02f) continue;

        // --- Its color ---
        float color[3];
        WordColor(s, i, f.words.size(), now, color);
        if (s.flowHighlight && static_cast<int>(i) == singing && leave >= 1.0f) {
            std::memcpy(color, s.flowHighlightColor, sizeof(color));
        }

        // --- Placing it ---
        const float size = w.size * grow;
        const float cellX = size / w.cells * scaleX * pulse * leaveScale;
        const float cellY = size / w.cells * scaleY * pulse * leaveScale;
        const float tilt = w.tilt + motionTilt + spin;
        const float cs = std::cos(tilt), sn = std::sin(tilt);
        const float wx = w.x * grow;
        const float wy = (w.y + dy + motionY + leaveDy) * grow;
        // A small push AWAY from the camera, for the outline and the shadow
        // (drawn just behind the letters, so they don't fight for the same depth).
        float away[3] = {0.0f, 0.0f, 0.0f};
        float dist = 30.0f;
        if (view.ok) {
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = anchor[k] - view.pos[k];
            dist = std::max(1.0f, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
            for (int k = 0; k < 3; ++k) away[k] = d[k] / dist;
        }
        // How many lines per pixel of the letter: enough to look solid (a
        // line is one screen pixel thick), no more. How big a pixel of the
        // letter looks depends on the distance AND on where it is on the
        // screen: near the edges the picture is stretched (perspective), so
        // a word at the top of the screen looks taller than the same word in
        // the middle. "depth" (the distance along where you look) is smaller
        // off-center: dist / depth^2 accounts for that stretch.
        int perCell = 3;
        if (view.ok) {
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = anchor[k] - view.pos[k];
            const float depth = std::max(1.0f, d[0] * view.forward[0] + d[1] * view.forward[1] + d[2] * view.forward[2]);
            const float cellPx = cellY * view.scaleY * displayH * 0.5f * dist / (depth * depth);
            perCell = std::clamp(static_cast<int>(std::ceil(cellPx * 1.3f)), 1, 14);
        }
        const float scatterAmount = size * 3.0f;
        auto place = [&](float px, float py, float push, float jx, float jy, float* outP) {
            const float lx = (px - w.width * 0.5f) * cellX + jx;
            const float ly = (w.cells * 0.5f - py) * cellY + jy;
            const float rx = lx * cs - ly * sn;  // the tilt, on the sign
            const float ry = lx * sn + ly * cs;
            for (int k = 0; k < 3; ++k) outP[k] = anchor[k] + f.right[k] * (wx + rx) + away[k] * push;
            outP[1] += wy + ry;
        };
        auto addRects = [&](const std::vector<Rect>& rects, float push, float shiftX, float shiftY,
                            sdk::draw3d::WorldShapes& shape) {
            shape.lines = true;
            shape.triangles.reserve(shape.triangles.size() + rects.size() * static_cast<std::size_t>(perCell + 2) * 6);
            for (std::size_t r = 0; r < rects.size(); ++r) {
                const Rect& rc = rects[r];
                if (rc.letter >= lettersShown) continue;
                float jx = 0.0f, jy = 0.0f;
                if (scatter > 0.0f) {
                    const float ang =
                        Hash01(static_cast<std::uint32_t>(i * 131 + r), static_cast<std::uint32_t>(f.index)) * 6.2831f;
                    jx = std::cos(ang) * scatter * scatterAmount;
                    jy = std::sin(ang) * scatter * scatterAmount;
                }
                float a[3], b[3];
                const float x0 = rc.x0 + shiftX, x1 = rc.x1 + shiftX, y0 = rc.y0 + shiftY, y1 = rc.y1 + shiftY;
                for (int k = 0; k < perCell; ++k) {
                    const float y = y0 + (static_cast<float>(k) + 0.5f) / static_cast<float>(perCell) * (y1 - y0);
                    place(x0, y, push, jx, jy, a);
                    place(x1, y, push, jx, jy, b);
                    shape.triangles.insert(shape.triangles.end(), a, a + 3);
                    shape.triangles.insert(shape.triangles.end(), b, b + 3);
                }
                place(x0, y0, push, jx, jy, a);
                place(x0, y1, push, jx, jy, b);
                shape.triangles.insert(shape.triangles.end(), a, a + 3);
                shape.triangles.insert(shape.triangles.end(), b, b + 3);
                place(x1, y0, push, jx, jy, a);
                place(x1, y1, push, jx, jy, b);
                shape.triangles.insert(shape.triangles.end(), a, a + 3);
                shape.triangles.insert(shape.triangles.end(), b, b + 3);
            }
        };
        const float behind = std::max(0.05f, cellY * 0.8f);
        if (s.flowShadow) {
            sdk::draw3d::WorldShapes shadow;
            shadow.color = sdk::draw3d::Color{0.0f, 0.0f, 0.0f, alpha * 0.55f};
            addRects(w.rects, behind * 2.0f, 0.9f, 0.9f, shadow);
            out.push_back(std::move(shadow));
        }
        if (s.flowOutline) {
            sdk::draw3d::WorldShapes border;
            border.color = sdk::draw3d::Color{s.flowOutlineColor[0], s.flowOutlineColor[1], s.flowOutlineColor[2],
                                              alpha * s.flowOutlineAlpha};
            addRects(w.outline, behind, 0.0f, 0.0f, border);
            out.push_back(std::move(border));
        }
        sdk::draw3d::WorldShapes letters;
        letters.color = sdk::draw3d::Color{color[0], color[1], color[2], alpha};
        addRects(w.rects, 0.0f, 0.0f, 0.0f, letters);
        out.push_back(std::move(letters));
    }
}

void Clear() {
    g_current = FlowLine{};
    g_previous = FlowLine{};
    ReleaseGdiFont();
    if (g_sent) {
        sdk::draw3d::SetWorldShapes({});
        g_sent = false;
    }
}

}  // namespace

void Draw(ImDrawList* draw, ImVec2 display) {
    (void)draw;
    const music::Settings& s = music::Get();
    const media::Track t = media::Current();
    const lyrics::Result& ly = music::CurrentLyrics();
    const bool have = t.present && ly.trackId == t.trackId && ly.status == lyrics::Status::Found && ly.synced &&
                      !ly.lines.empty();
    if (!have || !sdk::draw3d::Available()) {
        Clear();
        return;
    }
    const int ms = music::LyricsMs(t);
    const int index = lyrics::LineAt(ly, ms);
    const bool animate = music::LyricAnimations();
    const double now = rainbow::Seconds();

    // A new line: the old one starts leaving, the new one is written in front of you.
    const bool isPause =
        index >= 0 && ly.lines[static_cast<std::size_t>(index)].text.find_first_not_of(' ') == std::string::npos;
    // Letters settings changed (font, size, spacing...): write the line again.
    static std::uint64_t lastLook = 0;
    std::uint64_t look = static_cast<std::uint64_t>(s.flowFont) * 1000003u + static_cast<std::uint64_t>(s.flowResolution) * 7919u +
                         (s.flowBold ? 13u : 0u) + (s.flowUppercase ? 17u : 0u) +
                         static_cast<std::uint64_t>(std::lround(s.flowLetterSpacing * 10.0f + 50.0f)) * 104729u +
                         static_cast<std::uint64_t>(std::lround(s.flowWordSpacing * 100.0f)) * 1299709u +
                         static_cast<std::uint64_t>(std::lround(s.flowSize * 10.0f)) * 15485863u +
                         static_cast<std::uint64_t>(std::lround(s.flowTilt)) * 32452843u;
    if (look != lastLook) {
        lastLook = look;
        g_current.index = -2;  // forces a rebuild below
        g_previous = FlowLine{};
    }
    if (index != g_current.index || t.trackId != g_current.track) {
        // The old line should already be gone (it left before this one). If
        // it isn't (a seek, a very short gap), it finishes leaving quickly.
        if (g_current.index >= 0 && g_current.track == t.trackId && !g_current.words.empty() && ms < g_current.leaveEnd &&
            ms >= g_current.leaveStart - 50 && ms - g_current.leaveEnd < 0) {
            g_previous = std::move(g_current);
        } else {
            g_previous = FlowLine{};
        }
        g_current = FlowLine{};
        g_current.index = index;
        g_current.track = t.trackId;
        if (index >= 0 && !isPause) BuildLine(g_current, ly, index, t.trackId, s);
    }

    ViewInfo view;
    {
        sdk::camera::View v;
        if (sdk::camera::GetView(v)) {
            view.ok = true;
            std::memcpy(view.pos, v.pos, sizeof(view.pos));
            view.scaleY = v.scaleY;
            const float yaw = v.yaw * kDegToRad, pitch = v.pitch * kDegToRad;
            view.forward[0] = -std::sin(yaw) * std::cos(pitch);
            view.forward[1] = -std::sin(pitch);
            view.forward[2] = std::cos(yaw) * std::cos(pitch);
        }
    }
    // How much of a line is still there (1 = all, 0 = gone), by its schedule.
    auto leaveOf = [&](const FlowLine& f) {
        if (ms >= f.leaveEnd) return 0.0f;
        if (ms <= f.leaveStart) return 1.0f;
        if (!animate) return 1.0f;
        return 1.0f - Clamp01(static_cast<float>(ms - f.leaveStart) / static_cast<float>(std::max(1, f.leaveEnd - f.leaveStart)));
    };
    std::vector<sdk::draw3d::WorldShapes> shapes;
    if (!g_previous.words.empty()) {
        const float leave = leaveOf(g_previous);
        if (leave <= 0.0f) {
            g_previous = FlowLine{};
        } else {
            AddLine(shapes, g_previous, ms, leave, s, animate, view, display.y, now);
        }
    }
    if (!g_current.words.empty()) {
        const float leave = leaveOf(g_current);
        if (leave > 0.0f) AddLine(shapes, g_current, ms, leave, s, animate, view, display.y, now);
    }
    sdk::draw3d::SetWorldShapes(std::move(shapes));
    g_sent = true;
}

void Stop() {
    Clear();
}

}  // namespace luma::render::lyricflow
