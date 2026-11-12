// =============================================================================
//  hud.cpp - Implementación del estilo del HUD
// =============================================================================
#include "render/hud.h"
#include "sdk/game_crosshair.h"
#include "sdk/paperdoll.h"
#include "render/font_fallback.h"
#include "render/theme.h"
#include "render/rainbow.h"
#include "render/music.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cfloat>
#include <cmath>
#include <filesystem>

#include "core/logger.h"
#include "core/paths.h"

namespace luma::render::hud {

namespace {

Style g_style;
const Style* g_current = &g_style;  // el que se usa para dibujar

// La fuente pixelada se carga a 8 tamaños EXACTOS. Su cuadrícula: ImGui la
// escala para que (ascendente + descendente) = tamaño, y en esa fuente eso
// mide 11 "píxeles de letra" (9 arriba de la línea base + 2 abajo; las
// mayúsculas miden 7). Así que a tamaño 11*k cada píxel de letra mide
// exactamente k píxeles de pantalla: nítido, sin borroneo.
constexpr int kPixelSizes = 8;
constexpr float kPixelUnits = 11.0f;
std::array<ImFont*, kPixelSizes> g_pixel{};
bool g_pixelAvailable = false;

// La fuente "Luma" (suave): una sola, grande, que se achica al dibujar.
ImFont* g_smooth = nullptr;
float g_uiScale = 1.0f;

// La carpeta del juego: donde está Minecraft.Windows.exe (el proceso en el
// que estamos). La fuente está en <carpeta>\data\fonts\Mojangles.ttf.
std::filesystem::path GameFontPath() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path() / L"data" / L"fonts" / L"Mojangles.ttf";
}

ImU32 ToU32(const float rgb[3], float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
}

}  // namespace

Style& GetStyle() {
    return g_style;
}

bool PixelFontAvailable() {
    return g_pixelAvailable;
}

void LoadFonts(ImGuiIO& io, float uiScale) {
    g_uiScale = uiScale;
    g_pixel.fill(nullptr);
    g_smooth = nullptr;

    std::error_code ec;
    const std::filesystem::path pixelPath = GameFontPath();
    g_pixelAvailable = std::filesystem::exists(pixelPath, ec);
    if (g_pixelAvailable) {
        // Sin "sobremuestreo" ni suavizado de posición: para una fuente
        // pixelada queremos cada píxel en su lugar exacto.
        ImFontConfig config;
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.PixelSnapH = true;
        const std::string utf8 = paths::ToUtf8(pixelPath);
        for (int k = 1; k <= kPixelSizes; ++k) {
            g_pixel[k - 1] = io.Fonts->AddFontFromFileTTF(utf8.c_str(), kPixelUnits * static_cast<float>(k), &config);
            // Other alphabets (only the 4 smaller sizes: the bigger ones are
            // rarely used and would make the font picture much bigger).
            if (g_pixel[k - 1] != nullptr && k <= 4) fontfallback::MergeInto(io, kPixelUnits * static_cast<float>(k));
        }
        g_pixelAvailable = g_pixel[0] != nullptr;
    }

    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    const std::filesystem::path smoothPath = std::filesystem::path(windowsDir) / L"Fonts" / L"seguisb.ttf";
    if (std::filesystem::exists(smoothPath, ec)) {
        // Grande (36 px a 1080p): achicarla al dibujar se ve bien.
        g_smooth = io.Fonts->AddFontFromFileTTF(paths::ToUtf8(smoothPath).c_str(), 36.0f * uiScale);
        if (g_smooth != nullptr) fontfallback::MergeInto(io, 36.0f * uiScale);
    }
    log::Info("HUD: fuente pixelada del juego {}.", g_pixelAvailable ? "encontrada" : "no encontrada (se usa la de Luma)");
}

std::atomic<bool> g_f1Hidden{false};

// What we SAW the game do (preferred) and, only if we can't see it, the F1
// presses counted by hand.
bool g_gameHudKnown = false;
bool g_gameHudHidden = false;

void ToggleF1Hidden() {
    g_f1Hidden.store(!g_f1Hidden.load());
}

bool F1Hidden() {
    return g_gameHudKnown ? g_gameHudHidden : g_f1Hidden.load();
}

void UpdateGameHudState(bool playing, bool firstPerson) {
    static unsigned int last = 0;
    static int still = 0;
    // THE SIGNAL: how many times the game drew its HUD screen ("hud_screen",
    // sdk/paperdoll sees it). With F1 the game stops drawing it. (Before, it
    // was the crosshair's drawing; in 1.26 the crosshair is a screen of its
    // own and that signal stopped working.) Fallback: the crosshair.
    const bool screens = sdk::paperdoll::Available();
    if (!screens && !sdk::gamecrosshair::Available()) {
        g_gameHudKnown = false;
        return;
    }
    const unsigned int calls = screens ? sdk::paperdoll::HudScreenFrames() : sdk::gamecrosshair::RenderCalls();
    if (!playing || (!screens && !firstPerson)) {
        // In menus or in third person the game may not draw its crosshair
        // for other reasons: keep what we knew.
        last = calls;
        still = 0;
        return;
    }
    if (calls != last) {
        last = calls;
        still = 0;
        g_gameHudKnown = true;
        if (g_gameHudHidden) {
            g_gameHudHidden = false;
            log::Info("HUD: the game shows its HUD again: Luma's HUD comes back.");
        }
    } else if (++still >= 8 && !g_gameHudHidden) {
        g_gameHudKnown = true;
        g_gameHudHidden = true;
        log::Info("HUD: the game hid its HUD (F1): Luma's HUD hides too.");
    }
}

ImFont* PixelFont() {
    return g_pixelAvailable ? g_pixel[0] : nullptr;
}

ImFont* PixelFontAt(int k) {
    return g_pixelAvailable ? g_pixel[static_cast<std::size_t>(std::clamp(k, 1, kPixelSizes) - 1)] : nullptr;
}

ImFont* SmoothFont() {
    return g_smooth != nullptr ? g_smooth : ImGui::GetFont();
}

Metrics Measure(float scale) {
    Metrics m;
    // 2 píxeles de pantalla por píxel de HUD a 1080p y escala 1.
    const float target = 2.0f * g_uiScale * scale;
    const bool pixel = g_current->font == FontChoice::Pixel && g_pixelAvailable;
    if (pixel) {
        const int k = std::clamp(static_cast<int>(std::lround(target)), 1, kPixelSizes);
        m.font = g_pixel[k - 1];
        m.px = static_cast<float>(k);
        m.fontSize = kPixelUnits * m.px;
        m.capTop = 2.0f * m.px;
        m.capHeight = 7.0f * m.px;
    } else {
        m.font = g_smooth != nullptr ? g_smooth : ImGui::GetFont();
        m.px = target;
        m.fontSize = 12.5f * m.px;
        // Proporciones de Segoe UI (alto de mayúsculas y dónde empiezan).
        m.capTop = 0.285f * m.fontSize;
        m.capHeight = 0.526f * m.fontSize;
    }
    return m;
}

ImVec2 TextSize(const Metrics& m, const char* text) {
    return m.font->CalcTextSizeA(m.fontSize, FLT_MAX, 0.0f, text);
}

ImU32 TextColor(float alpha) {
    if (g_current->rainbow) {
        float c[3];
        rainbow::Color(g_current->rainbowSpeed, 0.0f, c[0], c[1], c[2]);
        return ToU32(c, alpha);
    }
    float album[3];
    if (music::HudTint(album)) {  // "Color the whole HUD" (Now playing module)
        return ToU32(album, alpha);
    }
    return ToU32(g_current->text, alpha);
}

ImU32 PressedColor(float alpha) {
    if (g_current->rainbow) {
        float c[3];
        rainbow::Color(g_current->rainbowSpeed, 0.5f, c[0], c[1], c[2]);  // (medio arcoíris corrido: se distingue)
        return ToU32(c, alpha);
    }
    float album[3];
    if (music::HudTint(album)) {
        return ToU32(album, alpha);
    }
    return ToU32(g_current->pressed, alpha);
}

namespace {
int g_ignoreEfficiency = 0;
}  // namespace

IgnoreEfficiency::IgnoreEfficiency() {
    ++g_ignoreEfficiency;
}
IgnoreEfficiency::~IgnoreEfficiency() {
    --g_ignoreEfficiency;
}

void Text(ImDrawList* draw, const Metrics& m, ImVec2 pos, const char* text, ImU32 color) {
    // Redondear a píxeles enteros: con la fuente pixelada, una posición con
    // decimales la dibuja "entre píxeles" y se ve borrosa.
    pos.x = std::floor(pos.x);
    pos.y = std::floor(pos.y);
    if (g_current->textShadow && (theme::HudShadowsOn() || g_ignoreEfficiency > 0)) {
        // La sombra es el mismo color, oscurecido al 25%, un píxel de HUD
        // abajo a la derecha (como la del juego).
        const ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
        const ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 0.25f, c.y * 0.25f, c.z * 0.25f, c.w));
        const float off = std::max(1.0f, std::floor(m.px));
        draw->AddText(m.font, m.fontSize, ImVec2(pos.x + off, pos.y + off), shadow, text);
    }
    draw->AddText(m.font, m.fontSize, pos, color, text);
}

void Box(ImDrawList* draw, const Metrics& m, ImVec2 min, ImVec2 max, float pressed) {
    min = ImVec2(std::floor(min.x), std::floor(min.y));
    max = ImVec2(std::floor(max.x), std::floor(max.y));
    const float rounding = g_current->rounding * m.px;
    if (g_current->background > 0.001f) {
        draw->AddRectFilled(min, max, ToU32(g_current->backgroundColor, g_current->background), rounding);
    }
    if (pressed > 0.001f) {
        // Tecla apretada: la caja se llena del color de "apretado".
        draw->AddRectFilled(min, max, PressedColor(0.85f * pressed), rounding);
    }
    if (g_current->border) {
        draw->AddRect(min, max, IM_COL32(255, 255, 255, 40), rounding, 0, std::max(1.0f, std::floor(m.px * 0.5f)));
    }
}

void CenteredText(ImDrawList* draw, const Metrics& m, ImVec2 min, ImVec2 max, const char* text, ImU32 color) {
    const ImVec2 size = TextSize(m, text);
    // Centrar las MAYÚSCULAS (lo que el ojo ve), no la línea entera con su
    // espacio para acentos y letras como la "g".
    const float x = min.x + (max.x - min.x - size.x) * 0.5f;
    const float y = min.y + (max.y - min.y - m.capHeight) * 0.5f - m.capTop;
    Text(draw, m, ImVec2(x, y), text, color);
}

float LineBoxHeight(const Metrics& m) {
    // 7 de mayúsculas + 5 arriba + 5 abajo = 17 píxeles de HUD.
    return m.capHeight + 10.0f * m.px;
}

ImVec2 TextBox(ImDrawList* draw, const Metrics& m, ImVec2 pos, const char* text, float minWidth) {
    const ImVec2 textSize = TextSize(m, text);
    const float width = std::floor(std::max(textSize.x + 10.0f * m.px, minWidth * m.px));
    const float height = std::floor(LineBoxHeight(m));
    const ImVec2 max(pos.x + width, pos.y + height);
    Box(draw, m, pos, max);
    CenteredText(draw, m, pos, max, text, TextColor());
    return ImVec2(width, height);
}

void UseStyle(const Style* style) {
    g_current = style != nullptr ? style : &g_style;
}

const Style& CurrentStyle() {
    return *g_current;
}

void SaveStyle(const Style& st, nlohmann::json& h) {
    h["font"] = st.font == FontChoice::Luma ? "luma" : "pixel";
    h["background"] = st.background;
    h["backgroundColor"] = {st.backgroundColor[0], st.backgroundColor[1], st.backgroundColor[2]};
    h["textShadow"] = st.textShadow;
    h["rounding"] = st.rounding;
    h["border"] = st.border;
    h["text"] = {st.text[0], st.text[1], st.text[2]};
    h["pressed"] = {st.pressed[0], st.pressed[1], st.pressed[2]};
    h["hideInGameMenus"] = st.hideInGameMenus;
    h["rainbow"] = st.rainbow;
    h["rainbowSpeed"] = st.rainbowSpeed;
}

void LoadStyle(const nlohmann::json& h, Style& st) {
    const Style defaults;
    st = defaults;
    if (!h.is_object()) {
        return;
    }
    st.font = h.value("font", std::string("pixel")) == "luma" ? FontChoice::Luma : FontChoice::Pixel;
    st.background = std::clamp(h.value("background", defaults.background), 0.0f, 1.0f);
    st.textShadow = h.value("textShadow", defaults.textShadow);
    st.rounding = std::clamp(h.value("rounding", defaults.rounding), 0.0f, 8.0f);
    st.border = h.value("border", defaults.border);
    st.hideInGameMenus = h.value("hideInGameMenus", defaults.hideInGameMenus);
    st.rainbow = h.value("rainbow", defaults.rainbow);
    st.rainbowSpeed = std::clamp(h.value("rainbowSpeed", defaults.rainbowSpeed), 0.05f, 2.0f);
    auto color = [&](const char* key, float* out) {
        if (h.contains(key) && h[key].is_array() && h[key].size() == 3) {
            for (int i = 0; i < 3; ++i) {
                out[i] = std::clamp(h[key][i].get<float>(), 0.0f, 1.0f);
            }
        }
    };
    color("backgroundColor", st.backgroundColor);
    color("text", st.text);
    color("pressed", st.pressed);
}

void Save(nlohmann::json& out) {
    nlohmann::json h;
    SaveStyle(g_style, h);
    out["hud"] = std::move(h);
}

void Load(const nlohmann::json& in) {
    if (in.is_object() && in.contains("hud")) {
        LoadStyle(in["hud"], g_style);
    } else {
        g_style = Style{};
    }
}

}  // namespace luma::render::hud
