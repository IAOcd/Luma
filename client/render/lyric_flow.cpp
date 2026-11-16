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
#include <utility>
#include <vector>

#include "core/lyrics.h"
#include "core/media.h"
#include "render/hud.h"
#include "render/music.h"
#include "render/rainbow.h"
#include "render/theme.h"
#include "core/tr.h"
#include "render/notifications.h"
#include "render/overlay.h"
#include "sdk/blocks.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"
#include "render/world_draw.h"
#include "sdk/mouse_grab.h"

namespace luma::render::lyricflow {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kPixelCells = 11.0f;     // Minecraft's pixel font: a line of text is 11 "pixels" tall
constexpr float kSkyDistance = 140.0f;   // "in the sky": this far away (blocks)
// The glow: how far it reaches (font pixels), in how many steps, and how
// fine its grid is (2 = half a font pixel: a smooth, round falloff).
constexpr float kGlowMax = 5.0f;
constexpr int kGlowBands = 8;
constexpr int kGlowSub = 2;

enum Appear { kPop, kFade, kDrop, kRise, kZoom, kSpin, kFlip, kTypewriter, kScatter };
enum Leave { kFloatUp, kFadeOut, kFall, kShrink, kScatterOut };
enum Motion { kNone, kFloat, kWave, kSway, kPulse };
enum Layout { kScattered, kCentered, kLeft, kArc, kStacked, kStaircase };

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
    // The glow: the space around the letters, in kGlowBands bands by how far
    // they are from the nearest letter (computed once per word: BuildGlow).
    std::vector<Rect> glow[kGlowBands];
    float left = 0, right = 0, top = 0, bottom = 0;  // where its lit pixels are (font pixels)
    // The karaoke fill: when it starts and how long it takes (song ms).
    int fillStart = 0;
    int fillDur = 300;
    bool burst = false;    // confetti: it already burst
    float emitAcc = 0.0f;  // particles: how many are "owed" (they come in fractions per frame)
};

// A particle (sparks, stardust...): it lives on the line's own flat space
// (the "sign": x to the right, y up, in blocks), so it follows the line.
struct Particle {
    float x = 0, y = 0, vx = 0, vy = 0;
    float life = 1, maxLife = 1;
    float size = 0.1f;     // in blocks
    int slot = 0;          // its color (see AddLine)
    float twinkle = 0.0f;  // 1 = it twinkles
    float phase = 0.0f;
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
    std::vector<Particle> parts;
    float emitAcc = 0.0f;
};

FlowLine g_current;
FlowLine g_previous;
int g_lineCount = 0;

// WHERE A LINE GOES. "base" = the sign's horizontal direction before the
// "Turn" setting (so turning can change without moving the place).
struct Place {
    bool ok = false;
    float offset[3]{};  // from the camera (the sky mode keeps only this direction)
    float anchor[3]{};  // in the world
    float base[3]{};    // the sign's horizontal direction, facing you
};
Place g_last;                  // where the last line was written
Place g_pin;                   // pinned: every line goes here (g_pin.ok)
std::uint32_t g_pinWorld = 0;  // the world it was pinned in (another world: unpinned)
bool g_pinKeyWasDown = true;   // (true: a key held while Luma loads isn't a press)

// The direction v turned by "deg" degrees around the vertical.
void TurnAround(const float v[3], float deg, float out[3]) {
    const float t = deg * kDegToRad, c = std::cos(t), sn = std::sin(t);
    out[0] = v[0] * c - v[2] * sn;
    out[1] = v[1];
    out[2] = v[0] * sn + v[2] * c;
}
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

// LETTERS THE FONT DOESN'T HAVE. Minecraft's pixel font (and most Windows
// fonts) has no Japanese, Chinese or Korean: those letters came out as empty
// boxes. A word with them is drawn with a Windows font that has them (a
// "fallback"), a little finer (16 pixels tall) so they stay readable.
constexpr const char* kFallbackFonts[] = {"Yu Gothic UI", "Malgun Gothic", "Segoe UI"};
constexpr int kFallbackBase = 100;  // (face numbers from 100 = kFallbackFonts)
enum Fallback { kFallJapanese = kFallbackBase, kFallKorean, kFallOther };

// Reads the next letter of a UTF-8 text (moves "p").
unsigned int NextChar(const char*& p, const char* end) {
    unsigned int c = 0;
    const int n = ImTextCharFromUtf8(&c, p, end);
    p += n > 0 ? n : 1;
    return c;
}
bool IsHangul(unsigned int c) {
    return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
}
// Japanese, Chinese or Korean (letters that are written without spaces).
bool IsCjk(unsigned int c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || IsHangul(c);
}
bool HasCjk(const std::string& text) {
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end) {
        if (IsCjk(NextChar(p, end))) return true;
    }
    return false;
}

bool EnsureGdiFont(int face, int cells, bool bold) {
    if (g_gdi.font != nullptr && g_gdi.face == face && g_gdi.cells == cells && g_gdi.bold == bold) return true;
    if (g_gdi.dc == nullptr) g_gdi.dc = CreateCompatibleDC(nullptr);
    if (g_gdi.dc == nullptr) return false;
    if (g_gdi.font != nullptr) {
        DeleteObject(g_gdi.font);
        g_gdi.font = nullptr;
    }
    wchar_t name[LF_FACESIZE]{};
    const char* faceName = face >= kFallbackBase ? kFallbackFonts[std::clamp(face - kFallbackBase, 0, 2)] : kFonts[face];
    MultiByteToWideChar(CP_UTF8, 0, faceName, -1, name, LF_FACESIZE);
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

bool PixelateGdi(Word& word, const std::string& text, const music::Settings& s, int face, int cells) {
    if (!EnsureGdiFont(face, cells, s.flowBold)) return false;
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

void PixelateLetters(Word& word, const std::string& text, const music::Settings& settings) {
    word.rects.clear();
    word.outline.clear();
    word.width = 0.0f;
    word.letters = 0;
    word.cells = kPixelCells;
    // Which font: Japanese, Chinese or Korean -> a fallback font that has them.
    // Minecraft's font also falls back (to Segoe UI) for any letter it lacks.
    int fallback = -1;
    {
        const char* p = text.c_str();
        const char* end = p + text.size();
        ImFont* pixel = hud::PixelFont();
        while (p < end) {
            const unsigned int c = NextChar(p, end);
            if (IsHangul(c)) {
                fallback = kFallKorean;
                break;
            }
            if (IsCjk(c)) {
                fallback = kFallJapanese;
                break;
            }
            if (settings.flowFont == 0 && pixel != nullptr && c > 0x7F && pixel->FindGlyphNoFallback(static_cast<ImWchar>(c)) == nullptr) {
                fallback = kFallOther;
            }
        }
    }
    if (fallback >= 0) {
        const int cells = std::max(16, std::clamp(settings.flowResolution, 9, 32));
        if (PixelateGdi(word, text, settings, fallback, cells)) return;
        word.rects.clear();
        word.outline.clear();
        word.letters = 0;
        word.cells = kPixelCells;
    }
    if (settings.flowFont != 0 &&
        PixelateGdi(word, text, settings, std::clamp(settings.flowFont, 1, kFontCount - 1), std::clamp(settings.flowResolution, 9, 32))) {
        return;
    }
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

// Rows of a fine grid -> rectangles ("unit" = how big one grid cell is, in
// font pixels).
void RunsScaled(const std::vector<std::uint8_t>& grid, int w, int h, float ox, float oy, float unit, std::vector<Rect>& out) {
    for (int r = 0; r < h; ++r) {
        int start = -1;
        for (int c = 0; c <= w; ++c) {
            const bool lit = c < w && grid[static_cast<std::size_t>(r) * w + c] != 0;
            if (lit && start < 0) start = c;
            if (!lit && start >= 0) {
                out.push_back(Rect{ox + static_cast<float>(start) * unit, oy + static_cast<float>(r) * unit,
                                   ox + static_cast<float>(c) * unit, oy + static_cast<float>(r + 1) * unit, 0});
                start = -1;
            }
        }
    }
}

// THE GLOW, the way a real glow fades: the word goes on a grid twice as fine
// as its pixels, and every empty cell gets its true distance to the nearest
// letter (round, not square: a "distance transform", two quick passes over
// the grid). Then the space around is cut in bands by that distance (a band
// every 5/8 of a pixel). Drawing each band a little more transparent than the
// one inside it gives a soft, round light around the letters. Done once per
// word; the drawing only picks how far and how bright (so the glow can grow
// and fade without computing anything again).
void BuildGlow(Word& w) {
    for (auto& g : w.glow) g.clear();
    if (w.rects.empty()) return;
    float minX = 1.0e9f, minY = 1.0e9f, maxX = -1.0e9f, maxY = -1.0e9f;
    for (const Rect& r : w.rects) {
        minX = std::min(minX, r.x0);
        minY = std::min(minY, r.y0);
        maxX = std::max(maxX, r.x1);
        maxY = std::max(maxY, r.y1);
    }
    w.left = minX;
    w.right = maxX;
    w.top = minY;
    w.bottom = maxY;
    constexpr int k = kGlowSub;
    const int margin = static_cast<int>(std::ceil(kGlowMax)) + 1;
    const int ox = static_cast<int>(std::floor(minX)) - margin, oy = static_cast<int>(std::floor(minY)) - margin;
    const int gw = (static_cast<int>(std::ceil(maxX)) + margin - ox) * k, gh = (static_cast<int>(std::ceil(maxY)) + margin - oy) * k;
    if (gw <= 0 || gh <= 0 || gw * gh > 600000) return;
    constexpr float kFar = 1.0e9f;
    std::vector<float> dist(static_cast<std::size_t>(gw) * gh, kFar);
    for (const Rect& r : w.rects) {
        const int x0 = static_cast<int>(std::floor((r.x0 - static_cast<float>(ox)) * k));
        const int x1 = static_cast<int>(std::ceil((r.x1 - static_cast<float>(ox)) * k));
        const int y0 = static_cast<int>(std::floor((r.y0 - static_cast<float>(oy)) * k));
        const int y1 = static_cast<int>(std::ceil((r.y1 - static_cast<float>(oy)) * k));
        for (int y = std::max(0, y0); y < std::min(gh, y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(gw, x1); ++x) dist[static_cast<std::size_t>(y) * gw + x] = 0.0f;
        }
    }
    // The two passes (top-left to bottom-right, then back): each cell takes
    // the smallest of its neighbors' distances plus the step to them.
    constexpr float kDiag = 1.41421356f;
    auto at = [&](int x, int y) -> float& { return dist[static_cast<std::size_t>(y) * gw + x]; };
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            float d = at(x, y);
            if (x > 0) d = std::min(d, at(x - 1, y) + 1.0f);
            if (y > 0) {
                d = std::min(d, at(x, y - 1) + 1.0f);
                if (x > 0) d = std::min(d, at(x - 1, y - 1) + kDiag);
                if (x + 1 < gw) d = std::min(d, at(x + 1, y - 1) + kDiag);
            }
            at(x, y) = d;
        }
    }
    for (int y = gh - 1; y >= 0; --y) {
        for (int x = gw - 1; x >= 0; --x) {
            float d = at(x, y);
            if (x + 1 < gw) d = std::min(d, at(x + 1, y) + 1.0f);
            if (y + 1 < gh) {
                d = std::min(d, at(x, y + 1) + 1.0f);
                if (x + 1 < gw) d = std::min(d, at(x + 1, y + 1) + kDiag);
                if (x > 0) d = std::min(d, at(x - 1, y + 1) + kDiag);
            }
            at(x, y) = d;
        }
    }
    const float step = kGlowMax * static_cast<float>(k) / static_cast<float>(kGlowBands);  // (grid cells)
    std::vector<std::uint8_t> band(dist.size());
    for (int b = 0; b < kGlowBands; ++b) {
        const float lo = step * static_cast<float>(b), hi = step * static_cast<float>(b + 1);
        for (std::size_t i = 0; i < dist.size(); ++i) band[i] = dist[i] > lo && dist[i] <= hi ? 1 : 0;
        RunsScaled(band, gw, gh, static_cast<float>(ox), static_cast<float>(oy), 1.0f / static_cast<float>(k), w.glow[b]);
    }
}

void Pixelate(Word& word, const std::string& text, const music::Settings& settings) {
    PixelateLetters(word, text, settings);
    BuildGlow(word);
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

struct Piece {
    std::string text;
    int ms;
};

// Lays a line's words out on its sign (and when it leaves). "lineMs" and
// "lineEnd": when the line starts and when the next one does.
void LayoutPieces(FlowLine& f, const std::vector<Piece>& pieces, int lineMs, int lineEnd, bool wordSynced,
                  const music::Settings& s, int index) {
    // Lay them out. "Scattered": rows where each word has its own size,
    // height and tilt (like the video). The others are tidy: every word the
    // same size, straight, arranged as the layout says (see the end).
    const bool scattered = s.flowLayout == kScattered;
    const float base = s.flowSize;
    const float maxRow = base * (s.flowLayout == kArc ? 18.0f : 13.0f);
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
        w.size = scattered ? base * (0.8f + 0.55f * r1 * r1) : base;
        w.tilt = scattered ? (r2 - 0.5f) * 2.0f * s.flowTilt * kDegToRad : 0.0f;
        w.phase = r3 * 6.2831f;
        const float widthBlocks = w.width / w.cells * w.size;
        const bool cjk = HasCjk(pieces[i].text);
        const bool cjkBefore = i > 0 && HasCjk(pieces[i - 1].text);
        const float gap = base * s.flowWordSpacing;
        if (cjk && cjkBefore && x > 0.0f) x -= gap * 0.85f;  // (the previous word's gap, almost all of it back)
        if (x > 0.0f && (x + widthBlocks > maxRow || s.flowLayout == kStacked || s.flowLayout == kStaircase)) {
            rows.push_back(Row{});
            rows.back().first = f.words.size();
            x = 0.0f;
        }
        w.x = x + widthBlocks * 0.5f;
        w.y = -static_cast<float>(rows.size() - 1) * base * 1.35f + (scattered ? (r3 - 0.5f) * base * 0.45f : 0.0f);
        x += widthBlocks + gap;
        rows.back().width = x - gap;
        ++rows.back().count;
        f.words.push_back(std::move(w));
    }
    const float totalH = static_cast<float>(rows.size() - 1) * base * 1.35f;
    float widest = 0.0f;
    for (const Row& r : rows) widest = std::max(widest, r.width);
    for (std::size_t ri = 0; ri < rows.size(); ++ri) {
        const Row& r = rows[ri];
        for (std::size_t i = r.first; i < r.first + r.count && i < f.words.size(); ++i) {
            Word& w = f.words[i];
            switch (s.flowLayout) {
                case kLeft:  // every row starts at the same left edge
                    w.x -= widest * 0.5f;
                    break;
                case kStaircase:  // each word a step to the right and down
                    w.x += (static_cast<float>(ri) - static_cast<float>(rows.size() - 1) * 0.5f) * base * 1.6f - r.width * 0.5f;
                    break;
                default:  // centered rows (scattered, centered, arc, stacked)
                    w.x -= r.width * 0.5f;
                    break;
            }
            w.y += totalH * 0.5f;
            if (s.flowLayout == kArc) {
                // Along a gentle arc (like a rainbow): the farther from the
                // center, the lower, and each word leans with the curve.
                const float radius = std::max(maxRow * 0.9f, base * 6.0f);
                const float angle = w.x / radius;
                w.x = radius * std::sin(angle);
                w.y += radius * (std::cos(angle) - 1.0f);
                w.tilt = -angle;
            }
        }
    }
    // WHEN IT LEAVES: it must be gone when the next line starts (so they never
    // overlap), so it starts leaving "leave time" before that. But never
    // before its last word was sung, and if a long instrumental comes, it
    // doesn't wait there forever: at most 2.5 s after its last word.
    // THE KARAOKE FILL: with word-by-word lyrics, each word fills while it's
    // sung. With line-by-line lyrics there are no word times, so the fill
    // runs across the whole line, each word in proportion to its width.
    if (wordSynced) {
        for (std::size_t i = 0; i < f.words.size(); ++i) {
            const int next = i + 1 < f.words.size() ? f.words[i + 1].ms : lineEnd;
            f.words[i].fillStart = f.words[i].ms;
            f.words[i].fillDur = std::clamp(next - f.words[i].ms, 120, 4000);
        }
    } else if (!f.words.empty()) {
        float total = 0.0f;
        for (const Word& w : f.words) total += std::max(1.0f, w.width);
        const float span = static_cast<float>(std::max(400, lineEnd - lineMs)) * 0.85f;
        float at = static_cast<float>(lineMs);
        for (Word& w : f.words) {
            const float d = span * std::max(1.0f, w.width) / total;
            w.fillStart = static_cast<int>(at);
            w.fillDur = std::max(120, static_cast<int>(d));
            at += d;
        }
    }
    const int leaveMs = static_cast<int>(s.flowLeaveTime * 1000.0f / std::max(0.3f, s.flowSpeed));
    int lastWord = lineMs, lastWordEnd = lineMs + 300;
    if (!f.words.empty()) {
        lastWord = f.words.back().ms;
        lastWordEnd = f.words.back().ms + f.words.back().duration;
    }
    f.leaveEnd = std::min(lineEnd, lastWordEnd + 2500 + leaveMs);
    f.leaveStart = std::max(lastWord + 150, f.leaveEnd - leaveMs);
    if (f.leaveStart > f.leaveEnd - 100) f.leaveStart = f.leaveEnd - 100;  // a very short gap: a quick exit
}

void BuildLine(FlowLine& f, const lyrics::Result& ly, int index, std::uint32_t track, const music::Settings& s) {
    f = FlowLine{};
    f.index = index;
    f.track = track;
    float pos[3], flat[3];
    if (!FlatView(pos, flat)) return;
    const lyrics::Line& line = ly.lines[static_cast<std::size_t>(index)];
    Place where;
    if (g_pin.ok) {
        where = g_pin;  // pinned: always the same place
    } else {
        // "Direction": the line goes that many degrees to your right (or
        // left) of where you look. Then a little to one side, then the other.
        const float a = s.flowAngle * kDegToRad;
        const float right0[3] = {-flat[2], 0.0f, flat[0]};
        float dir[3];
        for (int k = 0; k < 3; ++k) dir[k] = flat[k] * std::cos(a) + right0[k] * std::sin(a);
        where.base[0] = -dir[2];
        where.base[1] = 0.0f;
        where.base[2] = dir[0];
        const float side = (g_lineCount++ % 2 == 0 ? -1.0f : 1.0f) * s.flowDistance * s.flowSpread;
        for (int k = 0; k < 3; ++k) where.offset[k] = dir[k] * s.flowDistance + where.base[k] * side;
        where.offset[1] = s.flowHeight;
        for (int k = 0; k < 3; ++k) where.anchor[k] = pos[k] + where.offset[k];
        where.ok = true;
    }
    g_last = where;
    std::memcpy(f.offset, where.offset, sizeof(f.offset));
    std::memcpy(f.anchor, where.anchor, sizeof(f.anchor));
    TurnAround(where.base, s.flowTurn, f.right);  // "Turn": the sign turns sideways

    // The words (syllables joined), or the line split by spaces.
    std::vector<Piece> pieces;
    if (!line.words.empty()) {
        std::string word;
        int start = 0;
        for (std::size_t i = 0; i < line.words.size(); ++i) {
            if (word.empty()) start = line.words[i].ms;
            word += line.words[i].text;
            // (Japanese, Chinese... have no spaces: each timed piece is a word.)
            if (!word.empty() && (word.back() == ' ' || i + 1 == line.words.size() || HasCjk(line.words[i].text))) {
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
            if (HasCjk(w)) {
                // No spaces to split by: pieces of up to 4 letters.
                const char* q = w.c_str();
                const char* end = q + w.size();
                while (q < end) {
                    const char* start = q;
                    for (int k = 0; k < 4 && q < end; ++k) NextChar(q, end);
                    pieces.push_back(Piece{std::string(start, q), line.ms + 90 * i++});
                }
            } else if (!w.empty()) {
                pieces.push_back(Piece{w, line.ms + 90 * i++});
            }
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    const int lineEnd = index + 1 < static_cast<int>(ly.lines.size()) ? ly.lines[static_cast<std::size_t>(index + 1)].ms
                                                                       : line.ms + 4000;

    LayoutPieces(f, pieces, line.ms, lineEnd, !line.words.empty(), s, index);
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

// =============================================================================
//  DRAWING A LINE: layers, karaoke fill, glow and particles
// =============================================================================
//  Every word is drawn in LAYERS, back to front (the order matters when Luma
//  paints them; when the game draws them, each layer is a little farther
//  back, so the one in front always wins the depth):
//
//      shadow < glow (3 rings) < border < letters (not sung / sung) < edge < particles
//
//  The KARAOKE FILL splits the letters in two: the part already sung (its
//  color) and the part still to sing (a dimmer color). The split moves with
//  the song's time, like the lyrics of the menu: "Sweep" from left to right,
//  "Rise" from the bottom up, "Word at once", or "Bright edge" (a sweep
//  with a light band where it's filling). Nothing is cut in memory: each
//  rectangle is just clipped at the moving line while it's being drawn.
//
//  The GLOW: the game can't blur, so the glow is made of rings of pixels
//  around the letters (1, 2 and 3 pixels away, computed once per word),
//  each one more transparent: a soft pixel halo, Minecraft style.
//
//  PARTICLES: small squares that live a moment (sparks, stardust,
//  confetti, embers, fireflies). They live in the line's own flat space (the
//  "sign"), so they follow it wherever it is in the world.
//
//  WHERE IT ENDS: a "Sink". The world one turns the squares into lines (or
//  Luma's quads) for sdk/draw3d; the menu's preview one paints them flat.
//  Both get exactly the same squares: the preview is the real thing.
//  Shapes of the same layer and color are merged: one draw call each.
// =============================================================================

enum Layer {
    kLayerShadow,
    kLayerGlow3,
    kLayerGlow2,
    kLayerGlow1,
    kLayerOutline,
    kLayerUnsung,
    kLayerSung,
    kLayerEdge,
    kLayerParticles,
    kLayerCount
};
// How far behind the letters each layer goes (in "behind" units: see AddLine).
constexpr float kLayerPush[kLayerCount] = {2.2f, 2.0f, 1.8f, 1.6f, 1.0f, 0.0f, 0.0f, 0.0f, -0.4f};

enum Fill { kFillOff, kFillSweep, kFillWord, kFillRise, kFillEdge };
enum Glow { kGlowOff, kGlowSoft, kGlowBloom, kGlowNeon, kGlowFlash, kGlowPulse };
enum Vfx { kVfxOff, kVfxSparks, kVfxStardust, kVfxConfetti, kVfxEmbers, kVfxFireflies };

// Where the squares go. A quad is 4 corners (x, y pairs) on the sign, in
// blocks (x right, y up): top-left, top-right, bottom-right, bottom-left.
struct Sink {
    virtual ~Sink() = default;
    // A new line starts: where it is (only the world needs it).
    virtual void BeginLine(const float anchor[3], const float right[3], float lineBack, float pxPerBlock) {
        (void)anchor;
        (void)right;
        (void)lineBack;
        (void)pxPerBlock;
    }
    virtual void Quad(int layer, const sdk::draw3d::Color& color, const float* q, float push, bool solid) = 0;
};

bool SameColor(const sdk::draw3d::Color& a, const sdk::draw3d::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// --- The world -------------------------------------------------------------------------------
struct WorldSink final : Sink {
    const ViewInfo* view = nullptr;
    bool byLuma = false;
    float anchor[3]{}, right[3]{}, away[3]{};
    float lineBack = 0.0f, pxPerBlock = 0.0f;
    std::vector<sdk::draw3d::WorldShapes> layers[kLayerCount];

    void BeginLine(const float a[3], const float r[3], float back, float pxBlock) override {
        std::memcpy(anchor, a, sizeof(anchor));
        std::memcpy(right, r, sizeof(right));
        lineBack = back;
        pxPerBlock = pxBlock;
        away[0] = away[1] = away[2] = 0.0f;
        if (view != nullptr && view->ok) {
            float d[3], len = 0.0f;
            for (int k = 0; k < 3; ++k) {
                d[k] = anchor[k] - view->pos[k];
                len += d[k] * d[k];
            }
            len = std::sqrt(len);
            if (len > 0.001f) for (int k = 0; k < 3; ++k) away[k] = d[k] / len;
        }
    }
    sdk::draw3d::WorldShapes& ShapeFor(int layer, const sdk::draw3d::Color& color) {
        for (sdk::draw3d::WorldShapes& s : layers[layer]) {
            if (SameColor(s.color, color)) return s;
        }
        sdk::draw3d::WorldShapes& s = layers[layer].emplace_back();
        s.color = color;
        s.lines = !byLuma;
        s.quads = byLuma;
        return s;
    }
    // A point of the sign -> the world. "push" moves it away from the camera
    // along the line from the camera to the point (so the layer behind lands
    // exactly behind the same spot of the screen).
    void ToWorld(float x, float y, float push, float* out) const {
        for (int k = 0; k < 3; ++k) out[k] = anchor[k] + right[k] * x;
        out[1] += y;
        if (push == 0.0f) return;
        float ray[3];
        float len = 0.0f;
        if (view != nullptr && view->ok) {
            for (int k = 0; k < 3; ++k) ray[k] = out[k] - view->pos[k];
            len = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
        }
        for (int k = 0; k < 3; ++k) out[k] += (len > 0.001f ? ray[k] / len : away[k]) * push;
    }
    void Quad(int layer, const sdk::draw3d::Color& color, const float* q, float push, bool solid) override {
        sdk::draw3d::WorldShapes& shape = ShapeFor(layer, color);
        std::vector<float>& v = shape.triangles;
        if (byLuma) {
            // Luma paints it: the 4 corners (the order of the layers does the rest).
            float p[3];
            for (int c = 0; c < 4; ++c) {
                ToWorld(q[c * 2], q[c * 2 + 1], 0.0f, p);
                v.insert(v.end(), p, p + 3);
            }
            return;
        }
        // The game draws LINES: rows of thin lines across the square, enough
        // to look solid (a line is one screen pixel). See-through layers get
        // exactly one line per pixel and no side edges: overlapping lines
        // would add up and show as darker stripes.
        push += lineBack;
        const float hx = q[6] - q[0], hy = q[7] - q[1];
        const float px = std::sqrt(hx * hx + hy * hy) * pxPerBlock;
        int n = 3;
        if (pxPerBlock > 0.0f) {
            n = solid ? static_cast<int>(std::ceil(px * 1.3f)) : static_cast<int>(std::lround(px));
            n = std::clamp(n, 1, 14);
        }
        float a[3], b[3];
        for (int k = 0; k < n; ++k) {
            const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
            ToWorld(q[0] + (q[6] - q[0]) * t, q[1] + (q[7] - q[1]) * t, push, a);
            ToWorld(q[2] + (q[4] - q[2]) * t, q[3] + (q[5] - q[3]) * t, push, b);
            v.insert(v.end(), a, a + 3);
            v.insert(v.end(), b, b + 3);
        }
        if (solid) {
            for (int e = 0; e < 2; ++e) {
                const int top = e == 0 ? 0 : 2, bottom = e == 0 ? 6 : 4;
                ToWorld(q[top], q[top + 1], push, a);
                ToWorld(q[bottom], q[bottom + 1], push, b);
                v.insert(v.end(), a, a + 3);
                v.insert(v.end(), b, b + 3);
            }
        }
    }
    void Flush(std::vector<sdk::draw3d::WorldShapes>& out) {
        for (auto& layer : layers) {
            for (auto& s : layer) {
                if (!s.triangles.empty()) out.push_back(std::move(s));
            }
            layer.clear();
        }
    }
};

// --- The menu's preview ------------------------------------------------------------------------
struct PreviewSink final : Sink {
    ImDrawList* draw = nullptr;
    float cx = 0.0f, cy = 0.0f, scale = 1.0f;
    void Quad(int layer, const sdk::draw3d::Color& color, const float* q, float push, bool solid) override {
        (void)push;
        (void)solid;
        draw->ChannelsSetCurrent(layer);
        auto P = [&](int c) { return ImVec2(cx + q[c * 2] * scale, cy - q[c * 2 + 1] * scale); };
        draw->AddQuadFilled(P(0), P(1), P(2), P(3),
                            ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, std::clamp(color.a, 0.0f, 1.0f))));
    }
};

// --- A word's place on the sign ------------------------------------------------------------------
struct WordXf {
    float cellX = 1.0f, cellY = 1.0f;  // a font pixel, in blocks
    float cs = 1.0f, sn = 0.0f;        // its tilt
    float wx = 0.0f, wy = 0.0f;        // its center
    float halfW = 0.0f, halfH = 0.0f;  // (in font pixels)
    float rollCos = 1.0f, rollSin = 0.0f;
    // A point of the word (font pixels, y down) -> the sign, BEFORE the
    // line's rotation ("roll").
    void Flat(float px, float py, float jx, float jy, float& X, float& Y) const {
        const float lx = (px - halfW) * cellX + jx;
        const float ly = (halfH - py) * cellY + jy;
        X = wx + lx * cs - ly * sn;
        Y = wy + lx * sn + ly * cs;
    }
    void Roll(float X, float Y, float& x, float& y) const {
        x = X * rollCos - Y * rollSin;
        y = X * rollSin + Y * rollCos;
    }
    void Point(float px, float py, float jx, float jy, float* out) const {
        float X, Y;
        Flat(px, py, jx, jy, X, Y);
        Roll(X, Y, out[0], out[1]);
    }
};

// A group of rectangles of a word, drawn in one color and clipped to a box
// (the karaoke fill's moving split).
struct Batch {
    const std::vector<Rect>* rects = nullptr;
    int layer = kLayerSung;
    sdk::draw3d::Color color;
    float shiftX = 0.0f, shiftY = 0.0f;
    float x0 = -1.0e9f, x1 = 1.0e9f, y0 = -1.0e9f, y1 = 1.0e9f;
    bool solid = true;
};

void EmitBatch(Sink& sink, const Batch& b, const WordXf& xf, float behind, int lettersShown, float scatter,
               float scatterAmount, std::uint32_t seedA, std::uint32_t seedB) {
    if (b.color.a <= 0.005f) return;
    const std::vector<Rect>& rects = *b.rects;
    for (std::size_t r = 0; r < rects.size(); ++r) {
        const Rect& rc = rects[r];
        if (rc.letter >= lettersShown) continue;
        const float x0 = std::max(rc.x0 + b.shiftX, b.x0), x1 = std::min(rc.x1 + b.shiftX, b.x1);
        const float y0 = std::max(rc.y0 + b.shiftY, b.y0), y1 = std::min(rc.y1 + b.shiftY, b.y1);
        if (x1 - x0 <= 0.001f || y1 - y0 <= 0.001f) continue;
        float jx = 0.0f, jy = 0.0f;
        if (scatter > 0.0f) {
            const float ang = Hash01(seedA * 131u + static_cast<std::uint32_t>(r), seedB) * 6.2831f;
            jx = std::cos(ang) * scatter * scatterAmount;
            jy = std::sin(ang) * scatter * scatterAmount;
        }
        float q[8];
        xf.Point(x0, y0, jx, jy, q + 0);
        xf.Point(x1, y0, jx, jy, q + 2);
        xf.Point(x1, y1, jx, jy, q + 4);
        xf.Point(x0, y1, jx, jy, q + 6);
        sink.Quad(b.layer, b.color, q, kLayerPush[b.layer] * behind, b.solid);
    }
}

// --- Random numbers (fast, good enough for particles) ---------------------------------------------
float Rand01() {
    static std::uint32_t state = 0x1234567u;
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
}
float RandRange(float a, float b) {
    return a + (b - a) * Rand01();
}

// Particle colors, a few fixed "slots" (so particles of the same color are
// one draw call): 0 the letters, 1 lighter, 2 the album, 3 white, 4..9 a rainbow.
constexpr int kSlotLetters = 0, kSlotLight = 1, kSlotAlbum = 2, kSlotWhite = 3, kSlotRainbow = 4, kSlotCount = 10;
int PickSlot(const music::Settings& s, int style) {
    int mode = s.flowVfxColor;
    if (style == kVfxConfetti && mode == 0) mode = 2;  // (confetti is colorful unless you choose)
    switch (mode) {
        case 1: return kSlotAlbum;
        case 2: return kSlotRainbow + static_cast<int>(Rand01() * 5.99f);
        case 3: return kSlotWhite;
        default: return Rand01() < 0.5f ? kSlotLetters : kSlotLight;
    }
}

void Emit(FlowLine& f, float x, float y, float vx, float vy, float life, float size, int slot, float twinkle = 0.0f) {
    if (f.parts.size() >= 600) return;
    Particle p;
    p.x = x;
    p.y = y;
    p.vx = vx;
    p.vy = vy;
    p.life = p.maxLife = life;
    p.size = size;
    p.slot = slot;
    p.twinkle = twinkle;
    p.phase = Rand01() * 6.2831f;
    f.parts.push_back(p);
}

// Turns the line's visible words into squares for the sink.
void AddLine(Sink& sink, FlowLine& f, int ms, float leave, const music::Settings& s, bool animate, const ViewInfo& view,
             float displayH, double now, float dt) {
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
    const float rollCos = std::cos(s.flowRoll * kDegToRad), rollSin = std::sin(s.flowRoll * kDegToRad);
    const int fill = animate ? std::clamp(s.flowFill, 0, 4) : (s.flowFill != 0 ? kFillWord : kFillOff);
    const int glow = std::clamp(s.flowGlow, 0, 5);
    const int vfx = animate ? std::clamp(s.flowVfx, 0, 5) : kVfxOff;
    const bool byLuma = worlddraw::ByLuma();

    // How big a block looks on the screen, at the line (for how many lines a
    // square needs). Near the screen's edges the picture is stretched
    // (perspective): dist / depth^2 accounts for that.
    float pxPerBlock = 0.0f;
    if (view.ok) {
        float d[3];
        for (int k = 0; k < 3; ++k) d[k] = anchor[k] - view.pos[k];
        const float dist = std::max(1.0f, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        const float depth = std::max(1.0f, d[0] * view.forward[0] + d[1] * view.forward[1] + d[2] * view.forward[2]);
        pxPerBlock = view.scaleY * displayH * 0.5f * dist / (depth * depth);
    }
    // When the GAME draws them, everything has a depth: two things at the
    // same distance "fight" (stripes). The new line arrives where the old one
    // is still leaving, so the leaving line goes a bit farther back.
    const float lineBack = byLuma ? 0.0f : std::min(1.0f, raw * 8.0f) * std::max(0.6f, s.flowSize * grow * 0.5f);
    sink.BeginLine(anchor, f.right, lineBack, pxPerBlock);

    // The line's box (for particles), in the sign's own units (without "grow").
    float boxMin[2] = {1.0e9f, 1.0e9f}, boxMax[2] = {-1.0e9f, -1.0e9f};
    const float lineAlphaFade = std::clamp(1.0f - raw / 0.8f, 0.0f, 1.0f);

    for (std::size_t i = 0; i < f.words.size(); ++i) {
        Word& w = f.words[i];
        // Each word appears when it's sung. Or, with the karaoke fill and
        // "Whole line at once", the whole line is written at the start (a
        // quick cascade) and then filled word by word.
        const bool wholeLine = fill != kFillOff && s.flowFillWholeLine && !f.words.empty();
        const int appearAt = wholeLine ? f.words.front().ms + static_cast<int>(i) * 45 : w.ms;
        const float age = static_cast<float>(ms - appearAt) * speed;
        if (age < 0.0f) continue;
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
        // It fades a bit ahead of the movement (gone at 80% of the way).
        alpha *= (s.flowLeave == kShrink ? std::min(1.0f, lineAlphaFade * 2.0f) : lineAlphaFade) * s.flowOpacity;
        if (alpha <= 0.02f) continue;

        // --- How much of it was sung (0..1) ---
        const float sung = Clamp01(static_cast<float>(ms - w.fillStart) / static_cast<float>(std::max(1, w.fillDur)));
        const bool isSinging = ms >= w.fillStart && sung < 1.0f;  // (its fill is moving now)

        // --- Its colors ---
        float color[3];
        WordColor(s, i, f.words.size(), now, color);
        if (s.flowHighlight && static_cast<int>(i) == singing && leave >= 1.0f) {
            std::memcpy(color, s.flowHighlightColor, sizeof(color));
        }
        float dim[3];
        switch (s.flowUnsung) {
            case 1: dim[0] = dim[1] = dim[2] = 0.55f; break;
            case 2: dim[0] = dim[1] = dim[2] = 0.93f; break;
            case 3: std::memcpy(dim, s.flowUnsungColor, sizeof(dim)); break;
            default:
                for (int k = 0; k < 3; ++k) dim[k] = color[k] * 0.38f + 0.06f;
                break;
        }

        // --- Its place ---
        WordXf xf;
        const float size = w.size * grow;
        xf.cellX = size / w.cells * scaleX * pulse * leaveScale;
        xf.cellY = size / w.cells * scaleY * pulse * leaveScale;
        const float tilt = w.tilt + motionTilt + spin;
        xf.cs = std::cos(tilt);
        xf.sn = std::sin(tilt);
        xf.wx = w.x * grow;
        xf.wy = (w.y + dy + motionY + leaveDy) * grow;
        xf.halfW = w.width * 0.5f;
        xf.halfH = w.cells * 0.5f;
        xf.rollCos = rollCos;
        xf.rollSin = rollSin;
        const float behind = std::max(0.05f, xf.cellY * 0.8f);
        const float scatterAmount = size * 3.0f;
        const auto seed = static_cast<std::uint32_t>(i);
        const auto seedLine = static_cast<std::uint32_t>(f.index);
        auto emit = [&](const Batch& b) { EmitBatch(sink, b, xf, behind, lettersShown, scatter, scatterAmount, seed, seedLine); };

        // Where the fill splits the word (in font pixels).
        float cutX = 1.0e9f, cutY = -1.0e9f;  // (default: everything is "sung")
        bool before = false;                   // (the split is on x or on y)
        if (fill == kFillSweep || fill == kFillEdge) {
            cutX = w.left + (w.right - w.left) * sung;
            before = true;
        } else if (fill == kFillRise) {
            cutY = w.bottom - (w.bottom - w.top) * sung;
        } else if (fill == kFillWord) {
            if (ms < w.fillStart) cutX = -1.0e9f;
            before = true;
        }
        const bool anyUnsung = fill != kFillOff && sung < 1.0f;

        // 1. Shadow: dark, colored (a deep version of the letter's color), or
        // 3D (that colored copy twice, stepping out: the letters look thick).
        if (s.flowShadow) {
            Batch b;
            b.rects = &w.rects;
            b.layer = kLayerShadow;
            if (s.flowShadowStyle == 0) {
                b.color = sdk::draw3d::Color{0.0f, 0.0f, 0.0f, alpha * 0.55f};
                b.shiftX = b.shiftY = 0.9f;
                emit(b);
            } else {
                const float deep = s.flowShadowStyle == 2 ? 0.42f : 0.32f;
                b.color = sdk::draw3d::Color{color[0] * deep, color[1] * deep, color[2] * deep, alpha * 0.9f};
                b.shiftX = b.shiftY = 1.0f;
                emit(b);
                if (s.flowShadowStyle == 2) {
                    b.color = sdk::draw3d::Color{color[0] * 0.6f, color[1] * 0.6f, color[2] * 0.6f, alpha};
                    b.shiftX = b.shiftY = 0.5f;
                    emit(b);
                }
            }
        }
        // 2. Glow: the bands around the letters, each one more transparent
        // the farther it is (a soft, round falloff). The styles only change
        // how far it reaches, how bright its core is, and how it moves:
        //   Soft: calm and close. Bloom: wide and light, like an overexposed
        //   photo. Neon: a strong, saturated tube of light. Flash: a burst of
        //   light when the word is sung (with a light streak and a little
        //   star), that settles down. Pulse: it breathes.
        if (glow != kGlowOff) {
            const bool where = s.flowGlowWhere == 0 || s.flowGlowWhere == 2 || (s.flowGlowWhere == 1 && isSinging);
            const float since = static_cast<float>(ms - w.fillStart);  // ms since it started being sung
            const float burst = since >= 0.0f ? std::exp(-since / 420.0f) : 0.0f;
            if (where || (glow == kGlowFlash && burst > 0.02f)) {
                float gc[3];
                switch (s.flowGlowColor) {
                    case 1: std::memcpy(gc, color, sizeof(gc)); music::AlbumColor(gc); break;
                    case 2: std::memcpy(gc, s.flowGlowCustom, sizeof(gc)); break;
                    case 3: {
                        const double turns = now * 0.15 + static_cast<double>(i) * 0.13;
                        rainbow::Hsv(static_cast<float>(turns - std::floor(turns)), 0.7f, 1.0f, gc[0], gc[1], gc[2]);
                        break;
                    }
                    default: std::memcpy(gc, color, sizeof(gc)); break;
                }
                float radius = 3.2f, core = 0.42f, light = 0.3f;
                switch (glow) {
                    case kGlowBloom: radius = 5.0f; core = 0.5f; light = 0.6f; break;
                    case kGlowNeon: {
                        radius = 2.6f;
                        core = 0.75f;
                        light = 0.0f;
                        const float m = std::max({gc[0], gc[1], gc[2], 0.001f});
                        for (float& c : gc) c = std::clamp(c / m, 0.0f, 1.0f);
                        break;
                    }
                    case kGlowFlash:
                        radius = 1.8f + 3.2f * burst;
                        core = (since >= 0.0f ? 0.16f : 0.0f) + 0.8f * burst;
                        light = 0.35f + 0.5f * burst;
                        break;
                    case kGlowPulse: {
                        const float br = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 3.2f + w.phase);
                        radius = 2.6f + 1.4f * br;
                        core = 0.22f + 0.33f * br;
                        break;
                    }
                    default: break;
                }
                for (float& c : gc) c += (1.0f - c) * light;
                const float peak = std::clamp(core * s.flowGlowStrength, 0.0f, 1.0f) * alpha;
                if (peak > 0.01f) {
                    const float step = kGlowMax / static_cast<float>(kGlowBands);
                    for (int b = 0; b < kGlowBands; ++b) {
                        const float d = (static_cast<float>(b) + 0.5f) * step;
                        if (d >= radius) break;
                        const float k = 1.0f - d / radius;
                        // Quantized a little (so equal bands of different
                        // words share a draw call).
                        const float a = std::round(peak * k * k * 64.0f) / 64.0f;
                        if (a <= 0.0f) continue;
                        Batch band;
                        band.rects = &w.glow[b];
                        band.layer = kLayerGlow2;
                        band.solid = false;
                        band.color = sdk::draw3d::Color{gc[0], gc[1], gc[2], a};
                        if (anyUnsung && s.flowGlowWhere != 1) {
                            Batch lit = band;
                            if (before) lit.x1 = cutX; else lit.y0 = cutY;
                            emit(lit);
                            if (s.flowGlowWhere == 0) {
                                Batch rest = band;
                                rest.color.a = std::round(a * 0.35f * 64.0f) / 64.0f;
                                if (before) rest.x0 = cutX; else rest.y1 = cutY;
                                emit(rest);
                            }
                        } else {
                            emit(band);
                        }
                    }
                }
                // Flash: a streak of light across the word and a small star,
                // bright when the word starts and fading with the burst.
                if (glow == kGlowFlash && burst > 0.03f) {
                    constexpr int kLevels = 6;
                    static std::vector<Rect> flare[kLevels];
                    for (auto& v : flare) v.clear();
                    auto put = [&](float x0, float y0, float x1, float y1, float strength) {
                        const int lv = static_cast<int>(std::clamp(strength, 0.0f, 1.0f) * static_cast<float>(kLevels) + 0.5f) - 1;
                        if (lv >= 0) flare[std::min(lv, kLevels - 1)].push_back(Rect{x0, y0, x1, y1, 0});
                    };
                    const float midY = (w.top + w.bottom) * 0.5f;
                    const float cx = (w.left + w.right) * 0.5f;
                    const float half = (w.right - w.left) * 0.5f + 4.0f + 12.0f * burst;
                    for (float x = -half; x < half; x += 1.0f) {
                        const float fall = 1.0f - std::fabs(x + 0.5f) / half;
                        put(cx + x, midY - 0.3f, cx + x + 1.0f, midY + 0.3f, fall * fall * burst);
                        put(cx + x, midY - 0.9f, cx + x + 1.0f, midY - 0.3f, fall * fall * fall * burst * 0.45f);
                        put(cx + x, midY + 0.3f, cx + x + 1.0f, midY + 0.9f, fall * fall * fall * burst * 0.45f);
                    }
                    // The star: a little cross at the word's top corner.
                    const float sx = w.right - 0.5f, sy = w.top + 0.5f;
                    const float arm = 1.0f + 4.0f * burst;
                    for (float t = -arm; t < arm; t += 0.5f) {
                        const float fall = 1.0f - std::fabs(t + 0.25f) / arm;
                        put(sx + t, sy - 0.25f, sx + t + 0.5f, sy + 0.25f, fall * burst);
                        put(sx - 0.25f, sy + t, sx + 0.25f, sy + t + 0.5f, fall * burst);
                    }
                    float fc[3];
                    for (int c = 0; c < 3; ++c) fc[c] = gc[c] + (1.0f - gc[c]) * 0.7f;
                    for (int lv = 0; lv < kLevels; ++lv) {
                        if (flare[lv].empty()) continue;
                        Batch b;
                        b.rects = &flare[lv];
                        b.layer = kLayerParticles;
                        b.solid = false;
                        b.color = sdk::draw3d::Color{fc[0], fc[1], fc[2],
                                                     alpha * std::min(1.0f, static_cast<float>(lv + 1) / kLevels * s.flowGlowStrength)};
                        emit(b);
                    }
                }
            }
        }
        // 3. Border.
        if (s.flowOutline) {
            Batch b;
            b.rects = &w.outline;
            b.layer = kLayerOutline;
            b.color = sdk::draw3d::Color{s.flowOutlineColor[0], s.flowOutlineColor[1], s.flowOutlineColor[2],
                                         alpha * s.flowOutlineAlpha};
            emit(b);
        }
        // 4. Letters: the sung part, and the part still to sing.
        {
            Batch sungPart;
            sungPart.rects = &w.rects;
            sungPart.layer = kLayerSung;
            sungPart.color = sdk::draw3d::Color{color[0], color[1], color[2], alpha};
            if (!anyUnsung) {
                emit(sungPart);
            } else {
                Batch rest = sungPart;
                rest.layer = kLayerUnsung;
                rest.color = sdk::draw3d::Color{dim[0], dim[1], dim[2], alpha};
                if (before) {
                    const float edge = fill == kFillEdge ? std::min(1.6f, (w.right - w.left) * 0.25f) : 0.0f;
                    sungPart.x1 = cutX - edge;
                    rest.x0 = cutX;
                    emit(sungPart);
                    emit(rest);
                    if (edge > 0.0f && sung > 0.0f) {
                        // "Bright edge": a light band where it's filling.
                        Batch band = sungPart;
                        band.layer = kLayerEdge;
                        band.x0 = cutX - edge;
                        band.x1 = cutX;
                        band.color = sdk::draw3d::Color{color[0] + (1.0f - color[0]) * 0.75f, color[1] + (1.0f - color[1]) * 0.75f,
                                                        color[2] + (1.0f - color[2]) * 0.75f, alpha};
                        emit(band);
                    }
                } else {
                    sungPart.y0 = cutY;
                    rest.y1 = cutY;
                    emit(sungPart);
                    emit(rest);
                }
            }
        }

        // --- Particles: where they're born ---
        {
            // The word's box on the sign (without "grow": particles keep their size).
            float X0, Y0, X1, Y1;
            xf.Flat(w.left, w.bottom, 0.0f, 0.0f, X0, Y0);
            xf.Flat(w.right, w.top, 0.0f, 0.0f, X1, Y1);
            const float g = 1.0f / std::max(0.001f, grow);
            const float bx0 = std::min(X0, X1) * g, bx1 = std::max(X0, X1) * g;
            const float by0 = std::min(Y0, Y1) * g, by1 = std::max(Y0, Y1) * g;
            boxMin[0] = std::min(boxMin[0], bx0);
            boxMin[1] = std::min(boxMin[1], by0);
            boxMax[0] = std::max(boxMax[0], bx1);
            boxMax[1] = std::max(boxMax[1], by1);
            const float u = s.flowSize;           // a word's height: particles move in these units
            const float px = u / w.cells;         // a font pixel
            const float A = std::clamp(s.flowVfxAmount, 0.1f, 3.0f);
            if (vfx != kVfxOff && leave >= 1.0f && dt > 0.0f) {
                if (vfx == kVfxSparks && isSinging) {
                    // From where it's filling (or the word's right side).
                    w.emitAcc += dt * 45.0f * A;
                    while (w.emitAcc >= 1.0f) {
                        w.emitAcc -= 1.0f;
                        float ex, ey;
                        if (fill == kFillRise) {
                            xf.Flat(RandRange(w.left, w.right), cutY, 0.0f, 0.0f, ex, ey);
                        } else {
                            const float cx = (fill == kFillSweep || fill == kFillEdge) ? cutX : RandRange(w.left + (w.right - w.left) * 0.6f, w.right);
                            xf.Flat(cx, RandRange(w.top, w.bottom), 0.0f, 0.0f, ex, ey);
                        }
                        Emit(f, ex * g, ey * g, RandRange(-0.4f, 1.4f) * u, RandRange(0.8f, 2.6f) * u, RandRange(0.4f, 0.8f),
                             px * RandRange(0.6f, 1.0f), PickSlot(s, vfx));
                    }
                } else if (vfx == kVfxStardust && sung > 0.0f) {
                    w.emitAcc += dt * 9.0f * A;
                    while (w.emitAcc >= 1.0f) {
                        w.emitAcc -= 1.0f;
                        const float m = (by1 - by0) * 0.35f;
                        Emit(f, RandRange(bx0 - m, bx1 + m), RandRange(by0 - m, by1 + m), 0.0f, RandRange(0.02f, 0.12f) * u,
                             RandRange(0.8f, 1.6f), px * RandRange(0.5f, 0.9f), PickSlot(s, vfx), 1.0f);
                    }
                } else if (vfx == kVfxConfetti && sung >= 1.0f && !w.burst) {
                    // A burst when the word was just sung.
                    w.burst = true;
                    const int n = static_cast<int>(16.0f * A);
                    const float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
                    for (int k = 0; k < n; ++k) {
                        const float a = Rand01() * 6.2831f, v = RandRange(1.5f, 4.0f) * u;
                        Emit(f, cx, cy, std::cos(a) * v, std::sin(a) * v + u, RandRange(0.9f, 1.5f), px * RandRange(0.7f, 1.2f),
                             PickSlot(s, vfx));
                    }
                }
            }
            if (vfx == kVfxConfetti && sung >= 1.0f) w.burst = true;  // (no late bursts after a setting change)
        }

        // (Its box, for the next frame's particles: done.)
    }

    // --- Particles of the whole line ---
    if (vfx != kVfxOff && leave >= 1.0f && dt > 0.0f && boxMin[0] < boxMax[0]) {
        const float u = s.flowSize;
        const float px = u / kPixelCells;
        const float A = std::clamp(s.flowVfxAmount, 0.1f, 3.0f);
        if (vfx == kVfxEmbers) {
            f.emitAcc += dt * 14.0f * A;
            while (f.emitAcc >= 1.0f) {
                f.emitAcc -= 1.0f;
                Emit(f, RandRange(boxMin[0], boxMax[0]), boxMin[1] + RandRange(-0.2f, 0.3f) * u, RandRange(-0.2f, 0.2f) * u,
                     RandRange(0.5f, 1.2f) * u, RandRange(1.6f, 2.6f), px * RandRange(0.6f, 1.0f), PickSlot(s, vfx));
            }
        } else if (vfx == kVfxFireflies) {
            f.emitAcc += dt * 3.0f * A;
            while (f.emitAcc >= 1.0f) {
                f.emitAcc -= 1.0f;
                const float m = u * 1.2f;
                Emit(f, RandRange(boxMin[0] - m, boxMax[0] + m), RandRange(boxMin[1] - m, boxMax[1] + m), 0.0f, 0.0f,
                     RandRange(3.0f, 5.0f), px * RandRange(0.8f, 1.3f), PickSlot(s, vfx), 1.0f);
            }
        }
    }
    if (f.parts.empty()) return;

    // Move them (and remove the dead ones).
    if (dt > 0.0f) {
        const float u = s.flowSize;
        std::size_t live = 0;
        for (std::size_t k = 0; k < f.parts.size(); ++k) {
            Particle p = f.parts[k];
            p.life -= dt;
            if (p.life <= 0.0f) continue;
            switch (vfx) {
                case kVfxSparks: p.vy -= 4.5f * u * dt; break;
                case kVfxConfetti:
                    p.vy -= 5.0f * u * dt;
                    p.vx *= 1.0f - std::min(1.0f, 1.8f * dt);
                    break;
                case kVfxEmbers: p.vx = std::sin(static_cast<float>(now) * 2.2f + p.phase) * 0.35f * u; break;
                case kVfxFireflies:
                    p.vx = std::sin(static_cast<float>(now) * 0.9f + p.phase) * 0.45f * u;
                    p.vy = std::cos(static_cast<float>(now) * 0.7f + p.phase * 1.7f) * 0.35f * u;
                    break;
                default: break;
            }
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            f.parts[live++] = p;
        }
        f.parts.resize(live);
    }

    // Their colors (the slots).
    float slots[kSlotCount][3];
    {
        float c[3];
        WordColor(s, static_cast<std::size_t>(std::max(0, singing)), std::max<std::size_t>(1, f.words.size()), now, c);
        std::memcpy(slots[kSlotLetters], c, sizeof(c));
        for (int k = 0; k < 3; ++k) slots[kSlotLight][k] = c[k] + (1.0f - c[k]) * 0.6f;
        std::memcpy(slots[kSlotAlbum], c, sizeof(c));
        music::AlbumColor(slots[kSlotAlbum]);
        slots[kSlotWhite][0] = slots[kSlotWhite][1] = slots[kSlotWhite][2] = 1.0f;
        for (int k = 0; k < 6; ++k) {
            rainbow::Hsv(static_cast<float>(k) / 6.0f, 0.6f, 1.0f, slots[kSlotRainbow + k][0], slots[kSlotRainbow + k][1],
                         slots[kSlotRainbow + k][2]);
        }
    }
    // Draw them: tiny squares on the sign, a little in front of the letters.
    // Their transparency goes in 4 steps (so they share draw calls).
    const float lineAlpha = lineAlphaFade * s.flowOpacity;
    const float behindBase = std::max(0.05f, s.flowSize * grow / kPixelCells * 0.8f);
    for (const Particle& p : f.parts) {
        const float t = p.life / std::max(0.001f, p.maxLife);  // 1 -> 0
        float a = std::min(1.0f, t * 2.5f) * std::min(1.0f, (1.0f - t) * 8.0f + 0.2f);
        if (p.twinkle > 0.0f) a *= 0.35f + 0.65f * (0.5f + 0.5f * std::sin(static_cast<float>(now) * 9.0f + p.phase));
        a = std::round(a * lineAlpha * 4.0f) / 4.0f;
        if (a <= 0.0f) continue;
        const float* c = slots[std::clamp(p.slot, 0, kSlotCount - 1)];
        const float h = p.size * grow * 0.5f;
        const float X = p.x * grow, Y = p.y * grow;
        float q[8];
        const float corners[4][2] = {{X - h, Y + h}, {X + h, Y + h}, {X + h, Y - h}, {X - h, Y - h}};
        for (int k = 0; k < 4; ++k) {
            q[k * 2] = corners[k][0] * rollCos - corners[k][1] * rollSin;
            q[k * 2 + 1] = corners[k][0] * rollSin + corners[k][1] * rollCos;
        }
        sink.Quad(kLayerParticles, sdk::draw3d::Color{c[0], c[1], c[2], a}, q, kLayerPush[kLayerParticles] * behindBase, true);
    }
}

void Clear() {
    g_current = FlowLine{};
    g_previous = FlowLine{};
    ReleaseGdiFont();
    if (g_sent) {
        worlddraw::Submit({}, sdk::draw3d::kChannelLyrics);
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
    if (!have || !worlddraw::Ready()) {
        Clear();
        return;
    }
    const int ms = music::LyricsMs(t);
    const int index = lyrics::LineAt(ly, ms);
    const bool animate = music::LyricAnimations();
    const double now = rainbow::Seconds();

    // PAUSED: if the music stays paused for a while (10 s by default), the
    // lyrics leave the sky (with the leave effect). When it plays again, the
    // line being sung is written again, in front of where you look then.
    static double pausedSince = -1.0;
    static bool hiddenByPause = false;
    float pauseLeave = 1.0f;
    if (!t.playing) {
        if (pausedSince < 0.0) pausedSince = now;
        const double waited = now - pausedSince - static_cast<double>(s.flowPauseHide);
        if (s.flowPauseHide > 0.0f && waited > 0.0) {
            pauseLeave = animate ? 1.0f - Clamp01(static_cast<float>(waited) / std::max(0.15f, s.flowLeaveTime)) : 0.0f;
            if (pauseLeave <= 0.0f) {
                if (!hiddenByPause) {
                    hiddenByPause = true;
                    worlddraw::Submit({}, sdk::draw3d::kChannelLyrics);
                }
                return;
            }
        }
    } else {
        pausedSince = -1.0;
        if (hiddenByPause) {
            hiddenByPause = false;
            g_current.index = -2;  // write it again
            g_previous = FlowLine{};
        }
    }

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
                         static_cast<std::uint64_t>(std::lround(s.flowTilt)) * 32452843u +
                         static_cast<std::uint64_t>(s.flowLayout) * 49979687u +
                         static_cast<std::uint64_t>(std::lround(s.flowAngle + 400.0f)) * 86028121u +
                         static_cast<std::uint64_t>(std::lround(s.flowSpread * 100.0f)) * 122949829u +
                         static_cast<std::uint64_t>(std::lround(s.flowTurn + 400.0f)) * 179424691u +
                         // (where it is, so "Adjust in the world" moves the line right away)
                         static_cast<std::uint64_t>(std::lround(s.flowDistance * 4.0f)) * 252097800623u +
                         static_cast<std::uint64_t>(std::lround(s.flowHeight * 4.0f + 1000.0f)) * 3367900313u +
                         static_cast<std::uint64_t>(s.flowMode) * 5915587277u;
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
    // --- Pinning (the key in the settings) ---
    if (g_pin.ok && g_pinWorld != sdk::blocks::WorldSession()) g_pin = Place{};  // another world: not pinned
    {
        const bool down = s.flowPinKey != 0 && (GetAsyncKeyState(s.flowPinKey) & 0x8000) != 0;
        if (down && !g_pinKeyWasDown && sdk::mouse::GameMouseState() == 1 && !overlay::IsMenuOpen()) {
            SetPinned(!g_pin.ok);
        }
        g_pinKeyWasDown = down;
    }
    // How much of a line is still there (1 = all, 0 = gone), by its schedule.
    auto leaveOf = [&](const FlowLine& f) {
        if (ms >= f.leaveEnd) return 0.0f;
        if (ms <= f.leaveStart) return 1.0f;
        if (!animate) return 1.0f;
        return 1.0f - Clamp01(static_cast<float>(ms - f.leaveStart) / static_cast<float>(std::max(1, f.leaveEnd - f.leaveStart)));
    };
    // The time since the last frame (particles move with it).
    static double lastNow = 0.0;
    const float dt = lastNow > 0.0 ? static_cast<float>(std::clamp(now - lastNow, 0.0, 0.1)) : 0.0f;
    lastNow = now;
    WorldSink sink;
    sink.view = &view;
    sink.byLuma = worlddraw::ByLuma();
    if (!g_previous.words.empty()) {
        const float leave = std::min(leaveOf(g_previous), pauseLeave);
        if (leave <= 0.0f) {
            g_previous = FlowLine{};
        } else {
            AddLine(sink, g_previous, ms, leave, s, animate, view, display.y, now, dt);
        }
    }
    if (!g_current.words.empty()) {
        const float leave = std::min(leaveOf(g_current), pauseLeave);
        if (leave > 0.0f) AddLine(sink, g_current, ms, leave, s, animate, view, display.y, now, dt);
    }
    std::vector<sdk::draw3d::WorldShapes> shapes;
    sink.Flush(shapes);
    worlddraw::Submit(std::move(shapes), sdk::draw3d::kChannelLyrics);
    g_sent = true;
}

void Stop() {
    Clear();
}

bool Pinned() {
    return g_pin.ok;
}

void SetPinned(bool pinned) {
    if (!pinned) {
        if (g_pin.ok) notify::Push(notify::Kind::Info, Tr("Lyrics unpinned: they follow you again"));
        g_pin = Place{};
        return;
    }
    const music::Settings& s = music::Get();
    Place p;
    if (s.flowPinTarget == 0 && g_last.ok) {
        p = g_last;  // where the last line was
    } else {
        // Where you're looking now, at the chosen distance (also up or down).
        sdk::camera::View v;
        if (!sdk::camera::GetView(v)) return;
        const float yaw = v.yaw * kDegToRad, pitch = v.pitch * kDegToRad;
        const float fwd[3] = {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        for (int k = 0; k < 3; ++k) {
            p.offset[k] = fwd[k] * s.flowDistance;
            p.anchor[k] = v.pos[k] + p.offset[k];
        }
        p.base[0] = -std::cos(yaw);
        p.base[1] = 0.0f;
        p.base[2] = -std::sin(yaw);
        p.ok = true;
        // The lines on screen jump there too (so you see where it is).
        for (FlowLine* f : {&g_current, &g_previous}) {
            std::memcpy(f->offset, p.offset, sizeof(p.offset));
            std::memcpy(f->anchor, p.anchor, sizeof(p.anchor));
            TurnAround(p.base, s.flowTurn, f->right);
        }
    }
    g_pin = p;
    g_pinWorld = sdk::blocks::WorldSession();
    notify::Push(notify::Kind::Success, Tr("Lyrics pinned here"));
}

// --- The preview in the menu (Music > Settings > Lyric Flow) -------------------------
//  A sample line, laid out and drawn by EXACTLY the same code as in the world
//  (AddLine), only painted flat: appear, fill, glow, particles, leave... all
//  of it, on a loop. Built again when a letters or layout setting changes.
void DrawPreview(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    const music::Settings& s = music::Get();
    static FlowLine line;
    static std::uint64_t key = ~0ull;
    static double start = 0.0;
    static int lastMs = 0;
    static float box[4] = {0, 0, 1, 1};
    const std::uint64_t look = static_cast<std::uint64_t>(s.flowFont) * 1000003u + static_cast<std::uint64_t>(s.flowResolution) * 7919u +
                               (s.flowBold ? 13u : 0u) + (s.flowUppercase ? 17u : 0u) +
                               static_cast<std::uint64_t>(std::lround(s.flowLetterSpacing * 10.0f + 50.0f)) * 104729u +
                               static_cast<std::uint64_t>(std::lround(s.flowWordSpacing * 100.0f)) * 1299709u +
                               static_cast<std::uint64_t>(std::lround(s.flowSize * 10.0f)) * 15485863u +
                               static_cast<std::uint64_t>(std::lround(s.flowTilt)) * 32452843u +
                               static_cast<std::uint64_t>(s.flowLayout) * 49979687u +
                               static_cast<std::uint64_t>(std::lround(s.flowLeaveTime * 100.0f)) * 86028121u +
                               static_cast<std::uint64_t>(std::lround(s.flowSpeed * 10.0f)) * 122949829u;
    const double clock = ImGui::GetTime();
    if (look != key) {
        key = look;
        line = FlowLine{};
        line.index = 7;
        line.right[0] = 1.0f;
        const std::vector<Piece> pieces = {{s.flowUppercase ? "LUMA" : "Luma", 0},
                                           {s.flowUppercase ? "LYRIC" : "lyric", 650},
                                           {s.flowUppercase ? "FLOW" : "flow", 1300}};
        LayoutPieces(line, pieces, 0, 2600, true, s, 7);
        box[0] = box[1] = 1.0e9f;
        box[2] = box[3] = -1.0e9f;
        for (const Word& w : line.words) {
            const float hw = w.width / w.cells * w.size * 0.5f, hh = w.size * 0.5f;
            box[0] = std::min(box[0], w.x - hw);
            box[1] = std::min(box[1], w.y - hh);
            box[2] = std::max(box[2], w.x + hw);
            box[3] = std::max(box[3], w.y + hh);
        }
        start = clock;
    }
    if (line.words.empty()) return;
    // The loop: a moment of nothing, the line, its exit.
    const int period = line.leaveEnd + 900;
    const int ms = static_cast<int>(std::fmod((clock - start) * 1000.0, static_cast<double>(period))) - 500;
    if (ms < lastMs) {
        line.parts.clear();
        line.emitAcc = 0.0f;
        for (Word& w : line.words) {
            w.burst = false;
            w.emitAcc = 0.0f;
        }
    }
    lastMs = ms;
    float leave = 1.0f;
    if (ms >= line.leaveEnd) leave = 0.0f;
    else if (ms > line.leaveStart) leave = 1.0f - Clamp01(static_cast<float>(ms - line.leaveStart) / static_cast<float>(std::max(1, line.leaveEnd - line.leaveStart)));
    const float bw = std::max(0.1f, box[2] - box[0]), bh = std::max(0.1f, box[3] - box[1]);
    const float W = max.x - min.x, H = max.y - min.y;
    PreviewSink sink;
    sink.draw = draw;
    sink.scale = std::min(W * 0.8f / bw, H * 0.5f / bh);
    sink.cx = (min.x + max.x) * 0.5f - (box[0] + box[2]) * 0.5f * sink.scale;
    sink.cy = (min.y + max.y) * 0.5f + (box[1] + box[3]) * 0.5f * sink.scale;
    if (leave <= 0.0f && line.parts.empty()) return;
    draw->PushClipRect(min, max, true);
    draw->ChannelsSplit(kLayerCount);
    ViewInfo view;  // (no camera: flat)
    AddLine(sink, line, ms, std::max(leave, 0.001f), s, music::LyricAnimations(), view, 0.0f, rainbow::Seconds(),
            std::min(0.1f, ImGui::GetIO().DeltaTime));
    draw->ChannelsMerge();
    draw->PopClipRect();
}

}  // namespace luma::render::lyricflow
