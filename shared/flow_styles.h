// =============================================================================
//  flow_styles.h - Lyric Flow's ready-made styles (shared by Luma.dll and the
//  launcher, so both offer exactly the same ones)
// =============================================================================
//  A style changes only how the lyrics in the sky LOOK (colors, effects,
//  animations): never where they are, their size or their font. Each field
//  is the same setting as in render/music.h (flow...) and the same key in the
//  config (modules > now_playing > settings > lyricFlow), noted at its side.
// =============================================================================
#pragma once

namespace luma::flowstyles {

struct FlowLook {
    int colorMode;               // "colorMode": 0 one color, 1 album, 2 album gradient, 3 two colors, 4 rainbow
    float color[3], color2[3];   // "color", "color2"
    bool outline;                // "outline"
    bool shadow;                 // "shadow"
    int shadowStyle;             // "shadowStyle"
    int appear, leave, motion;   // "appear", "leave", "motion"
    float speed;                 // "speed"
    int fill, unsung, glow, glowColor;  // "fill", "unsung", "glow", "glowColor"
    float glowStrength;          // "glowStrength"
    int glowWhere, vfx;          // "glowWhere", "vfx"
    float vfxAmount;             // "vfxAmount"
    int vfxColor;                // "vfxColor"
};

// The names are English keys (each side translates them).
inline constexpr const char* kNames[] = {"Karaoke", "Neon", "Party", "Dream", "Fire", "Minimal"};
inline constexpr FlowLook kStyles[] = {
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
inline constexpr int kCount = static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0]));

// What a style also puts back to Lyric Flow's defaults (render/music.h).
inline constexpr float kDefaultOpacity = 0.95f;           // "opacity"
inline constexpr float kDefaultOutlineColor[3] = {0.05f, 0.05f, 0.07f};  // "outlineColor"
inline constexpr float kDefaultOutlineAlpha = 0.85f;      // "outlineAlpha"

}  // namespace luma::flowstyles
