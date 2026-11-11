// =============================================================================
//  theme.cpp - Implementación del tema
// =============================================================================
#include "render/theme.h"

#include "render/music.h"

#include <algorithm>
#include <atomic>

namespace luma::theme {

namespace {

Settings g_settings;
Fonts g_fonts;
float g_uiScale = 1.0f;
std::atomic<bool> g_fontRebuild{false};

// Las dos paletas. Los valores con alfa bajo dejan ver el vidrio (y el juego
// desenfocado) a través.
const Palette kDarkPalette{
    /*glassTint*/ ImVec4(0.075f, 0.082f, 0.090f, 0.55f),
    /*surface*/ ImVec4(1.0f, 1.0f, 1.0f, 0.055f),
    /*surfaceHover*/ ImVec4(1.0f, 1.0f, 1.0f, 0.10f),
    /*field*/ ImVec4(1.0f, 1.0f, 1.0f, 0.075f),
    /*text*/ kIvory,
    /*textDim*/ ImVec4(0.914f, 0.902f, 0.875f, 0.55f),
    /*border*/ ImVec4(1.0f, 1.0f, 1.0f, 0.16f),
    /*highlight*/ ImVec4(1.0f, 1.0f, 1.0f, 0.10f),
    /*shadow*/ ImVec4(0.0f, 0.0f, 0.0f, 0.35f),
    /*trackOff*/ ImVec4(1.0f, 1.0f, 1.0f, 0.14f),
    /*knob*/ ImVec4(0.98f, 0.97f, 0.95f, 1.0f),
    /*onAccentText*/ kCharcoal,
};

const Palette kLightPalette{
    /*glassTint*/ ImVec4(0.945f, 0.937f, 0.914f, 0.58f),
    /*surface*/ ImVec4(1.0f, 1.0f, 1.0f, 0.38f),
    /*surfaceHover*/ ImVec4(1.0f, 1.0f, 1.0f, 0.58f),
    /*field*/ ImVec4(1.0f, 1.0f, 1.0f, 0.45f),
    /*text*/ kCharcoal,
    /*textDim*/ ImVec4(0.145f, 0.157f, 0.169f, 0.55f),
    /*border*/ ImVec4(1.0f, 1.0f, 1.0f, 0.70f),
    /*highlight*/ ImVec4(1.0f, 1.0f, 1.0f, 0.45f),
    /*shadow*/ ImVec4(0.0f, 0.0f, 0.0f, 0.18f),
    /*trackOff*/ ImVec4(0.145f, 0.157f, 0.169f, 0.16f),
    /*knob*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*onAccentText*/ kIvory,
};

}  // namespace

Settings& Get() {
    return g_settings;
}

const Palette& P() {
    return g_settings.style == Style::Light ? kLightPalette : kDarkPalette;
}

Fonts& GetFonts() {
    return g_fonts;
}

void SetUiScale(float scale) {
    g_uiScale = scale;
}

float UiScale() {
    return g_uiScale;
}

ImVec4 Accent(float alpha) {
    float album[3];
    if (render::music::MenuTint(album)) {  // "Color Luma's menu" (Now playing module)
        return ImVec4(album[0], album[1], album[2], alpha);
    }
    if (g_settings.accentAuto) {
        // El color de Luma que contrasta con el estilo.
        const ImVec4 c = g_settings.style == Style::Light ? kCharcoal : kIvory;
        return ImVec4(c.x, c.y, c.z, alpha);
    }
    return ImVec4(g_settings.accent[0], g_settings.accent[1], g_settings.accent[2], alpha);
}

ImU32 AccentU32(float alpha) {
    return ImGui::ColorConvertFloat4ToU32(Accent(alpha));
}

void Apply() {
    // Empezamos de un estilo NUEVO cada vez. (Si modificáramos el actual,
    // ScaleAllSizes multiplicaría los tamaños una y otra vez.)
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    const Palette& p = P();

    const float rounding = g_settings.rounding;
    style.WindowRounding = rounding;
    style.ChildRounding = rounding * 0.75f;
    style.FrameRounding = rounding * 0.5f;
    style.PopupRounding = rounding * 0.6f;
    style.GrabRounding = rounding * 0.5f;
    style.TabRounding = rounding * 0.5f;
    style.ScrollbarRounding = rounding;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 8.0f;
    style.GrabMinSize = 14.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;

    // Opaco: el color que se ve "a través" de un control translúcido sobre el
    // vidrio. Para los popups (combos, tooltips) usamos un fondo casi opaco.
    const ImVec4 popup = g_settings.style == Style::Light ? ImVec4(0.96f, 0.955f, 0.94f, 0.98f)
                                                            : ImVec4(0.11f, 0.12f, 0.13f, 0.98f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.textDim;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);  // el menú dibuja sus propios paneles de vidrio
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = popup;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_FrameBg] = p.field;
    c[ImGuiCol_FrameBgHovered] = WithAlpha(p.field, 1.6f);
    c[ImGuiCol_FrameBgActive] = WithAlpha(p.field, 2.0f);
    c[ImGuiCol_Button] = p.field;
    c[ImGuiCol_ButtonHovered] = WithAlpha(p.field, 1.7f);
    c[ImGuiCol_ButtonActive] = WithAlpha(p.field, 2.2f);
    c[ImGuiCol_Header] = p.field;
    c[ImGuiCol_HeaderHovered] = WithAlpha(p.field, 1.6f);
    c[ImGuiCol_HeaderActive] = WithAlpha(p.field, 2.0f);
    c[ImGuiCol_CheckMark] = Accent(1.0f);
    c[ImGuiCol_SliderGrab] = Accent(1.0f);
    c[ImGuiCol_SliderGrabActive] = Accent(1.0f);
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = WithAlpha(p.text, 0.18f);
    c[ImGuiCol_ScrollbarGrabHovered] = WithAlpha(p.text, 0.30f);
    c[ImGuiCol_ScrollbarGrabActive] = WithAlpha(p.text, 0.45f);
    c[ImGuiCol_PlotLines] = Accent(1.0f);
    c[ImGuiCol_PlotHistogram] = Accent(1.0f);
    c[ImGuiCol_TextSelectedBg] = Accent(0.30f);
    c[ImGuiCol_NavCursor] = Accent(1.0f);
    c[ImGuiCol_Tab] = p.field;
    c[ImGuiCol_TabHovered] = WithAlpha(p.field, 1.7f);
    c[ImGuiCol_TabSelected] = WithAlpha(p.field, 2.2f);

    style.ScaleAllSizes(g_uiScale);
    ImGui::GetStyle() = style;
}

void RequestFontRebuild() {
    g_fontRebuild.store(true);
}

bool ConsumeFontRebuildRequest() {
    return g_fontRebuild.exchange(false);
}

void Save(nlohmann::json& out) {
    nlohmann::json t;
    t["style"] = g_settings.style == Style::Light ? "light" : "dark";
    t["accentAuto"] = g_settings.accentAuto;
    t["accent"] = {g_settings.accent[0], g_settings.accent[1], g_settings.accent[2]};
    t["glassOpacity"] = g_settings.glassOpacity;
    t["blur"] = g_settings.blur;
    t["rounding"] = g_settings.rounding;
    t["fontSize"] = g_settings.fontSize;
    t["animations"] = g_settings.animations;
    t["sounds"] = g_settings.sounds;
    t["soundVolume"] = g_settings.soundVolume;
    t["backgroundDim"] = g_settings.backgroundDim;
    t["moduleView"] = g_settings.moduleView;
    out["theme"] = std::move(t);
}

void Load(const nlohmann::json& in) {
    const Settings defaults;
    const float oldFont = g_settings.fontSize;
    g_settings = defaults;
    if (in.is_object() && in.contains("theme") && in["theme"].is_object()) {
        const nlohmann::json& t = in["theme"];
        g_settings.style = t.value("style", std::string("dark")) == "light" ? Style::Light : Style::Dark;
        g_settings.accentAuto = t.value("accentAuto", defaults.accentAuto);
        if (t.contains("accent") && t["accent"].is_array() && t["accent"].size() == 3) {
            for (int i = 0; i < 3; ++i) {
                g_settings.accent[i] = std::clamp(t["accent"][i].get<float>(), 0.0f, 1.0f);
            }
        }
        g_settings.glassOpacity = std::clamp(t.value("glassOpacity", defaults.glassOpacity), 0.1f, 0.95f);
        g_settings.blur = std::clamp(t.value("blur", defaults.blur), 0.0f, 1.0f);
        g_settings.rounding = std::clamp(t.value("rounding", defaults.rounding), 0.0f, 28.0f);
        g_settings.fontSize = std::clamp(t.value("fontSize", defaults.fontSize), 13.0f, 28.0f);
        g_settings.animations = t.value("animations", defaults.animations);
        g_settings.sounds = t.value("sounds", defaults.sounds);
        g_settings.soundVolume = std::clamp(t.value("soundVolume", defaults.soundVolume), 0.0f, 1.0f);
        g_settings.backgroundDim = std::clamp(t.value("backgroundDim", defaults.backgroundDim), 0.0f, 0.8f);
        g_settings.moduleView = std::clamp(t.value("moduleView", defaults.moduleView), 0, 1);
    }
    if (g_settings.fontSize != oldFont) {
        RequestFontRebuild();
    }
}

}  // namespace luma::theme
