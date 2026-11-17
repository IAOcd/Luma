// =============================================================================
//  theme.cpp - Las dos paletas (ver theme.h)
// =============================================================================
#include "gui/theme.h"

#include <algorithm>

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
Palette g_live = kDark;   // the palette in use: the theme's, with the accent of the moment
bool g_hasAccent = false;
ImVec4 g_accent{};

float Luminance(const ImVec4& c) {
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// THE CHANGE BETWEEN DARK AND LIGHT is soft: the palette in use is a mix of
// both (0 = dark, 1 = light) that slides toward the chosen one in about half
// a second (TickTheme, each frame). Every color of the launcher comes from it,
// so everything changes together.
float g_mix = 0.0f;

// A theme's palette, with the accent of the moment adjusted to read on it.
Palette Build(ThemeKind kind) {
    Palette out = kind == ThemeKind::Light ? kLight : kDark;
    if (!g_hasAccent) return out;
    ImVec4 c = g_accent;
    c.w = 1.0f;
    const float l = Luminance(c);
    if (kind == ThemeKind::Dark && l < 0.55f) {
        // Too dark for a dark window: toward white until it reads.
        const float k = (0.55f - l) / std::max(0.001f, 1.0f - l);
        c = ImVec4(c.x + (1.0f - c.x) * k, c.y + (1.0f - c.y) * k, c.z + (1.0f - c.z) * k, 1.0f);
    } else if (kind == ThemeKind::Light && l > 0.42f) {
        const float k = 0.42f / l;
        c = ImVec4(c.x * k, c.y * k, c.z * k, 1.0f);
    }
    out.accent = c;
    out.onAccent = Luminance(c) > 0.5f ? Hex(0x1B1D1F) : Hex(0xF4F1EA);
    return out;
}

float Eased() {
    return g_mix * g_mix * (3.0f - 2.0f * g_mix);
}

void Rebuild() {
    const Palette a = Build(ThemeKind::Dark), b = Build(ThemeKind::Light);
    const float t = Eased();
    static_assert(sizeof(Palette) % sizeof(ImVec4) == 0, "the palette is only colors");
    const ImVec4* pa = reinterpret_cast<const ImVec4*>(&a);
    const ImVec4* pb = reinterpret_cast<const ImVec4*>(&b);
    ImVec4* out = reinterpret_cast<ImVec4*>(&g_live);
    for (std::size_t i = 0; i < sizeof(Palette) / sizeof(ImVec4); ++i) {
        out[i] = ImVec4(pa[i].x + (pb[i].x - pa[i].x) * t, pa[i].y + (pb[i].y - pa[i].y) * t, pa[i].z + (pb[i].z - pa[i].z) * t,
                        pa[i].w + (pb[i].w - pa[i].w) * t);
    }
}

}  // namespace

void SetTheme(ThemeKind kind) {
    static bool first = true;
    g_theme = kind;
    if (first) {  // (at start: straight to it, no slide)
        first = false;
        g_mix = kind == ThemeKind::Light ? 1.0f : 0.0f;
    }
    Rebuild();
}

void TickTheme(float dt) {
    const float target = g_theme == ThemeKind::Light ? 1.0f : 0.0f;
    if (g_mix == target) return;
    const float step = dt * 2.4f;  // (about 0.4 s)
    g_mix = g_mix < target ? std::min(target, g_mix + step) : std::max(target, g_mix - step);
    Rebuild();
}

float ThemeLight() {
    return Eased();
}

ThemeKind CurrentTheme() {
    return g_theme;
}

const Palette& P() {
    return g_live;
}

void SetAccent(const ImVec4* color) {
    g_hasAccent = color != nullptr;
    if (color != nullptr) g_accent = *color;
    Rebuild();
}

}  // namespace luma::launcher::gui
