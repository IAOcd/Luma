// =============================================================================
//  theme.cpp - Las dos paletas (ver theme.h)
// =============================================================================
#include "gui/theme.h"

namespace luma::launcher::gui {

namespace {

constexpr ImVec4 Hex(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

constexpr Palette kDark{
    Hex(0x1B1D1F),        // background
    Hex(0x202326),        // sidebar
    Hex(0x25282B),        // card (el carbón del logo)
    Hex(0x2D3135),        // cardHover
    Hex(0xE9E6DF, 0.07f), // border
    Hex(0xE9E6DF),        // text (el marfil del logo)
    Hex(0xE9E6DF, 0.55f), // textDim
    Hex(0xE9E6DF),        // accent
    Hex(0x25282B),        // onAccent
    Hex(0x2E3236),        // heroTop
    Hex(0x1E2023),        // heroBottom
    Hex(0xE9E6DF, 0.06f), // heroShape
    Hex(0x7FB08A),        // success
    Hex(0xD6AE5E),        // warning
    Hex(0xD07468),        // danger
};

constexpr Palette kLight{
    Hex(0xEFEDE8),        // background
    Hex(0xE5E2DB),        // sidebar
    Hex(0xF8F7F4),        // card
    Hex(0xFFFFFF),        // cardHover
    Hex(0x25282B, 0.08f), // border
    Hex(0x25282B),        // text
    Hex(0x25282B, 0.58f), // textDim
    Hex(0x25282B),        // accent
    Hex(0xE9E6DF),        // onAccent
    Hex(0xE9E6DF),        // heroTop
    Hex(0xD9D5CC),        // heroBottom
    Hex(0x25282B, 0.07f), // heroShape
    Hex(0x4E8A5C),        // success
    Hex(0xA9802F),        // warning
    Hex(0xB04E43),        // danger
};

ThemeKind g_theme = ThemeKind::Dark;

}  // namespace

void SetTheme(ThemeKind kind) {
    g_theme = kind;
}

ThemeKind CurrentTheme() {
    return g_theme;
}

const Palette& P() {
    return g_theme == ThemeKind::Light ? kLight : kDark;
}

}  // namespace luma::launcher::gui
