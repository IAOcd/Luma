// =============================================================================
//  theme.cpp - Implementación del tema
// =============================================================================
#include "render/theme.h"

#include <algorithm>
#include <atomic>

namespace luma::theme {

namespace {
Settings g_settings;
Fonts g_fonts;
float g_uiScale = 1.0f;
std::atomic<bool> g_fontRebuild{false};
}  // namespace

Settings& Get() {
    return g_settings;
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

    const float rounding = g_settings.rounding;
    style.WindowRounding = rounding;
    style.ChildRounding = rounding * 0.8f;
    style.FrameRounding = rounding * 0.6f;
    style.PopupRounding = rounding * 0.6f;
    style.GrabRounding = rounding * 0.6f;
    style.TabRounding = rounding * 0.6f;
    style.ScrollbarRounding = rounding;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(12.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = style.Colors;
    const float alpha = g_settings.windowOpacity;
    auto withAlpha = [](ImVec4 color, float a) { color.w = a; return color; };
    c[ImGuiCol_Text] = palette::kText;
    c[ImGuiCol_TextDisabled] = palette::kTextDim;
    c[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);  // el menú dibuja sus propios paneles
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = withAlpha(palette::kPanelDark, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_TitleBg] = palette::kPanelDark;
    c[ImGuiCol_TitleBgActive] = palette::kPanelDark;
    c[ImGuiCol_FrameBg] = palette::kTile;
    c[ImGuiCol_FrameBgHovered] = palette::kTileHover;
    c[ImGuiCol_FrameBgActive] = withAlpha(palette::kField, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.15f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    c[ImGuiCol_ScrollbarGrabActive] = Accent(0.8f);
    // La opacidad del tema se aplica a los paneles que dibuja el menú.
    (void)alpha;
    c[ImGuiCol_Button] = Accent(0.22f);
    c[ImGuiCol_ButtonHovered] = Accent(0.45f);
    c[ImGuiCol_ButtonActive] = Accent(0.65f);
    c[ImGuiCol_Header] = Accent(0.22f);
    c[ImGuiCol_HeaderHovered] = Accent(0.40f);
    c[ImGuiCol_HeaderActive] = Accent(0.55f);
    c[ImGuiCol_CheckMark] = Accent(1.0f);
    c[ImGuiCol_SliderGrab] = Accent(1.0f);
    c[ImGuiCol_SliderGrabActive] = Accent(1.0f);
    c[ImGuiCol_Separator] = Accent(0.25f);
    c[ImGuiCol_Tab] = Accent(0.15f);
    c[ImGuiCol_TabHovered] = Accent(0.45f);
    c[ImGuiCol_TabSelected] = Accent(0.35f);
    c[ImGuiCol_PlotLines] = Accent(1.0f);
    c[ImGuiCol_PlotHistogram] = Accent(1.0f);
    c[ImGuiCol_TextSelectedBg] = Accent(0.35f);
    c[ImGuiCol_NavCursor] = Accent(1.0f);

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
    t["accent"] = {g_settings.accent[0], g_settings.accent[1], g_settings.accent[2]};
    t["windowOpacity"] = g_settings.windowOpacity;
    t["rounding"] = g_settings.rounding;
    t["fontSize"] = g_settings.fontSize;
    t["animations"] = g_settings.animations;
    t["sounds"] = g_settings.sounds;
    t["soundVolume"] = g_settings.soundVolume;
    t["backgroundDim"] = g_settings.backgroundDim;
    out["theme"] = std::move(t);
}

void Load(const nlohmann::json& in) {
    Settings defaults;
    const float oldFont = g_settings.fontSize;
    g_settings = defaults;
    if (in.is_object() && in.contains("theme") && in["theme"].is_object()) {
        const nlohmann::json& t = in["theme"];
        if (t.contains("accent") && t["accent"].is_array() && t["accent"].size() == 3) {
            for (int i = 0; i < 3; ++i) {
                g_settings.accent[i] = std::clamp(t["accent"][i].get<float>(), 0.0f, 1.0f);
            }
        }
        g_settings.windowOpacity = std::clamp(t.value("windowOpacity", defaults.windowOpacity), 0.3f, 1.0f);
        g_settings.rounding = std::clamp(t.value("rounding", defaults.rounding), 0.0f, 16.0f);
        g_settings.fontSize = std::clamp(t.value("fontSize", defaults.fontSize), 13.0f, 28.0f);
        g_settings.animations = t.value("animations", defaults.animations);
        g_settings.sounds = t.value("sounds", defaults.sounds);
        g_settings.soundVolume = std::clamp(t.value("soundVolume", defaults.soundVolume), 0.0f, 1.0f);
        g_settings.backgroundDim = std::clamp(t.value("backgroundDim", defaults.backgroundDim), 0.0f, 0.8f);
    }
    if (g_settings.fontSize != oldFont) {
        RequestFontRebuild();
    }
}

}  // namespace luma::theme
