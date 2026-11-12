// =============================================================================
//  lyric_flow.h - Lyric Flow: the lyrics floating IN the world (music module)
// =============================================================================
//  While a song with word-by-word lyrics plays, each line is written in the
//  sky in front of you, one word at a time, exactly when it's sung: words of
//  different sizes, a little tilted, popping in. The line STAYS where it was
//  written (it's anchored to a place of the world): turn your head and it
//  stays there, like a sign in the sky. The next line is written where you're
//  looking then.
//
//  The letters are REAL geometry of the world (made of small squares, like
//  pixel art, with the game's own pixel font): the game draws them together
//  with the world (sdk/draw3d.h), so a mountain in front covers them.
//
//  Lines without word times appear whole (word after word, quickly).
//  COST: only while it's on and lyrics are playing: a few dozen words a frame.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::lyricflow {

// The fonts Lyric Flow can write with. The first one is Minecraft's own pixel
// font; the rest are fonts that come with Windows, "pixelated" at the chosen
// resolution (Windows draws each letter, Luma keeps its lit cells).
inline constexpr const char* kFonts[] = {
    "Minecraft", "Segoe UI", "Segoe UI Black", "Impact", "Arial Black", "Georgia", "Times New Roman", "Consolas",
    "Courier New", "Comic Sans MS", "Segoe Script", "Segoe Print", "Trebuchet MS", "Verdana", "Bahnschrift",
};
inline constexpr int kFontCount = static_cast<int>(sizeof(kFonts) / sizeof(kFonts[0]));

// Called by the music module every frame Lyric Flow is on.
void Draw(ImDrawList* draw, ImVec2 display);
// Lyric Flow turned off (or the module): remove the letters from the world.
void Stop();

}  // namespace luma::render::lyricflow
