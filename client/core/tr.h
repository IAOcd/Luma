// =============================================================================
//  tr.h - Translating Luma's in-game texts
// =============================================================================
//  The launcher uses keys ("home.play" -> "PLAY"), see shared/i18n.h. The DLL
//  has hundreds of texts spread across the modules ("Show the name",
//  "Text color"...), and giving each one a key would make the code hard to
//  read. So here the "key" IS the text itself, written in English:
//
//      ui::Toggle(Tr("Show the name"), &showName_);
//
//  Tr() looks that text up in the table (core/tr_table.inc, one line per text
//  with Spanish, English and Portuguese) and returns the one for the chosen
//  language. A row can be found by its English OR its Spanish text. If it's
//  not in the table, it returns the same text (so nothing ever breaks: at
//  worst you see an untranslated text).
//
//  The controls in render/ui.h already call Tr() by themselves, so the
//  modules' options get translated without touching each module.
//
//  Cost: one lookup in a hash table (short text). Only with the menu open or
//  for a few HUD texts: nothing in the hot path.
// =============================================================================
#pragma once

#include <format>
#include <string>

namespace luma {

const char* Tr(const char* text);

// Like std::format, but translating the format first:
//     TrFormat("Profile created: {}", name)
template <typename... Args>
std::string TrFormat(const char* format, Args&&... args) {
    return std::vformat(Tr(format), std::make_format_args(args...));
}

}  // namespace luma
