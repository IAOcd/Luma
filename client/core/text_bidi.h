// =============================================================================
//  text_bidi.h - Arabic and Hebrew: right to left, and joined Arabic letters
// =============================================================================
//  Two things make Arabic (and Persian, Urdu) and Hebrew special:
//    1. They're read RIGHT TO LEFT. ImGui only draws left to right, so the
//       letters must be put in "visual order" first (the last one first).
//       Numbers and Latin words inside stay left to right.
//    2. Arabic letters JOIN: each letter has up to 4 shapes (alone, at the
//       start, in the middle or at the end of a word). Fonts keep those
//       shapes as separate characters ("presentation forms"); Luma picks the
//       right one for each letter ("shaping").
//  This is a simple version of what Windows or a browser do (the full
//  Unicode algorithm is much bigger): enough for song lyrics and titles.
// =============================================================================
#pragma once

#include <string>

namespace luma::bidi {

// Does the text have right-to-left letters (Arabic, Hebrew)?
bool HasRtl(const std::string& utf8);

// The text ready to draw left to right: Arabic letters joined, and the
// right-to-left parts in visual order. Texts without RTL letters come back as they are.
std::string Visual(const std::string& utf8);

}  // namespace luma::bidi
