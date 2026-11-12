// =============================================================================
//  pixel_art.h - Drawing made of Minecraft pixels (maps and their windows)
// =============================================================================
//  The minimap, the waypoints in the world and the world map share these
//  pieces. Everything sits on a grid of "q" screen pixels (one pixel of the
//  game's interface), in the game's own font, so nothing looks blurry and it
//  feels like Minecraft: flat colors, a lighter top edge and a darker bottom
//  one (like the game's buttons), hard shadows one pixel down-right.
//
//  The windows (panels, buttons, check boxes, text fields) are Luma's own
//  drawing in that style: gray stone panels, buttons that light up under the
//  mouse, and the game's font.
// =============================================================================
#pragma once

#include <imgui.h>

#include <cstdint>

namespace luma::render::pixel {

// The game's font with q screen pixels per font pixel (or the smooth one).
struct PixelText {
    ImFont* font = nullptr;
    float size = 0, capTop = 0, capHeight = 0, q = 1;
    bool pixel = false;
};
PixelText TextAt(int qi);
// A text's visible width (the pixel font leaves one pixel after each letter).
float TextWidth(const PixelText& t, const char* text);
// Text whose capitals start at "pos.y", with the game's text shadow.
void Text(ImDrawList* draw, const PixelText& t, ImVec2 pos, const char* text, ImU32 color, bool shadow = true);

// A color (3 floats 0..1) lighter (k > 1) or darker (k < 1).
ImU32 Shade(const float* rgb, float k, int alpha);

// The waypoint banner: its tip touches "tip"; the initial of "name" inside.
void Banner(ImDrawList* draw, ImVec2 tip, const PixelText& t, const float* rgb, int alpha, const char* name);
// A line of text like the game's name tags (white on a dark see-through
// band), centered on "cx", its bottom at "bottom", kept between minX and maxX.
void NameTag(ImDrawList* draw, float cx, float bottom, const PixelText& t, const char* text, ImU32 color, float minX, float maxX);

// A radar icon: a pixel dot of "size" (2..6) pixels, round (0), a diamond (1)
// or a square (2), with a dark outline.
void Dot(ImDrawList* draw, ImVec2 center, float q, ImU32 color, int shape, int size);

// --- Minecraft-style windows ------------------------------------------------------
// A stone panel (light gray, a white top-left edge, a dark bottom-right one,
// a black outline), like the game's inventory.
void Panel(ImDrawList* draw, ImVec2 min, ImVec2 max, float q);
// A dark see-through panel (like the chat's background), for lists over the map.
void DarkPanel(ImDrawList* draw, ImVec2 min, ImVec2 max, float q);
// A button like the game's: gray stone, lit when the mouse is over it.
// Uses an ImGui item (id) so it takes the click. True when clicked.
bool Button(const char* id, const char* label, ImVec2 pos, ImVec2 size, const PixelText& t, bool selected = false);
// A check box with its label to the right. True when changed.
bool Check(const char* id, const char* label, bool* value, ImVec2 pos, const PixelText& t);
// A text field (black inside, gray outline, the game's font). True when changed.
bool Field(const char* id, char* buffer, int size, ImVec2 pos, float width, const PixelText& t);

// --- A FORM: settings one under the other, in the dark sidebar style ------------
// Dark see-through rows, white text with the game's shadow, and the THEME'S
// ACCENT COLOR for what's on or selected (switches, slider fill, the chosen
// option). It lays itself out from top to bottom ("y" grows); rows outside
// [clipTop, clipBottom] are skipped (no mouse, no drawing), so a long form
// can scroll. Every control returns true when the user changed it.
struct Form {
    ImDrawList* draw = nullptr;
    PixelText text;           // labels
    PixelText hint;           // hints and values (smaller)
    float x = 0, width = 0;   // the column
    float y = 0;              // where the next row goes
    float clipTop = 0, clipBottom = 1e9f;
    ImU32 accent = IM_COL32(240, 160, 64, 255);

    void Section(const char* title);
    void Hint(const char* text);
    bool Toggle(const char* label, bool* value);
    bool Slider(const char* label, float* value, float min, float max, const char* format);
    bool SliderInt(const char* label, int* value, int min, int max, const char* format);
    bool Choice(const char* label, int* value, const char* const* options, int count);
    // A color: the 16 Minecraft dyes as squares (and the current one marked).
    bool Colors(const char* label, float* rgb);
    bool ButtonRow(const char* label, bool primary = false);
    // A text field for the whole width (search, names...).
    bool Input(const char* id, char* buffer, int size, const char* placeholder);
    void Space(float units);

    float q() const { return text.q; }
    bool Visible(float h) const { return y + h >= clipTop && y <= clipBottom; }
};

// The 16 colors of Minecraft's dyes (white, orange, magenta, light blue...).
extern const float kDyes[16][3];

}  // namespace luma::render::pixel
