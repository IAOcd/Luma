// =============================================================================
//  font_fallback.h - Letters of every language (Japanese, Korean, Chinese,
//                    Arabic, Hebrew, Russian, Thai, Hindi...)
// =============================================================================
//  ImGui keeps every letter it can draw in one picture (the "font atlas"),
//  made when the fonts are loaded. Luma's fonts only had the Latin letters,
//  so lyrics in other alphabets showed "?" boxes.
//
//  Putting ALL the letters of Chinese, Japanese and Korean in that picture
//  would need tens of thousands of letters at every size: a huge picture.
//  So Luma does it on demand: when a song (its title or its lyrics) uses
//  letters that aren't there yet, they're added and the fonts are rebuilt
//  once (a tiny pause, once per new set of letters). Each alphabet comes from
//  a Windows font that has it ("fallback fonts"):
//    Japanese  -> Yu Gothic (then the Chinese ones, for rare kanji)
//    Korean    -> Malgun Gothic
//    Chinese   -> Microsoft YaHei, then JhengHei (traditional)
//    Thai      -> Leelawadee UI
//    Hindi...  -> Nirmala UI
//    the rest  -> Segoe UI, Tahoma, Segoe UI Symbol (Arabic, Hebrew,
//                 Cyrillic, Greek, accents of every Latin language, symbols)
//  Arabic and Hebrew are also written right to left: see core/text_bidi.h.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::fontfallback {

// This text will be shown: make sure its letters exist (UTF-8). Cheap when
// they already do. If some are new, the fonts are rebuilt on the next frame.
void Need(const char* utf8);

// Called by the font loader right after adding each font: merges the letters
// that were needed into it, from the fallback fonts.
void MergeInto(ImGuiIO& io, float size);

// Called once at the start of every font build (fixes the lists for it).
void BeginBuild();

}  // namespace luma::render::fontfallback
