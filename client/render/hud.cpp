// =============================================================================
//  hud.cpp - Implementación del estilo del HUD
// =============================================================================
#include "render/hud.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <filesystem>

#include "core/logger.h"
#include "core/paths.h"

namespace luma::render::hud {

namespace {

Style g_style;

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
        }
        g_pixelAvailable = g_pixel[0] != nullptr;
    }

    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    const std::filesystem::path smoothPath = std::filesystem::path(windowsDir) / L"Fonts" / L"seguisb.ttf";
    if (std::filesystem::exists(smoothPath, ec)) {
        // Grande (36 px a 1080p): achicarla al dibujar se ve bien.
        g_smooth = io.Fonts->AddFontFromFileTTF(paths::ToUtf8(smoothPath).c_str(), 36.0f * uiScale);
    }
    log::Info("HUD: fuente pixelada del juego {}.", g_pixelAvailable ? "encontrada" : "no encontrada (se usa la de Luma)");
}

Metrics Measure(float scale) {
    Metrics m;
    // 2 píxeles de pantalla por píxel de HUD a 1080p y escala 1.
    const float target = 2.0f * g_uiScale * scale;
    const bool pixel = g_style.font == FontChoice::Pixel && g_pixelAvailable;
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
    return ToU32(g_style.text, alpha);
}

ImU32 PressedColor(float alpha) {
    return ToU32(g_style.pressed, alpha);
}

void Text(ImDrawList* draw, const Metrics& m, ImVec2 pos, const char* text, ImU32 color) {
    // Redondear a píxeles enteros: con la fuente pixelada, una posición con
    // decimales la dibuja "entre píxeles" y se ve borrosa.
    pos.x = std::floor(pos.x);
    pos.y = std::floor(pos.y);
    if (g_style.textShadow) {
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
    const float rounding = g_style.rounding * m.px;
    if (g_style.background > 0.001f) {
        draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, static_cast<int>(g_style.background * 255.0f)), rounding);
    }
    if (pressed > 0.001f) {
        // Tecla apretada: la caja se llena del color de "apretado".
        draw->AddRectFilled(min, max, PressedColor(0.85f * pressed), rounding);
    }
    if (g_style.border) {
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

void Save(nlohmann::json& out) {
    nlohmann::json h;
    h["font"] = g_style.font == FontChoice::Luma ? "luma" : "pixel";
    h["background"] = g_style.background;
    h["textShadow"] = g_style.textShadow;
    h["rounding"] = g_style.rounding;
    h["border"] = g_style.border;
    h["text"] = {g_style.text[0], g_style.text[1], g_style.text[2]};
    h["pressed"] = {g_style.pressed[0], g_style.pressed[1], g_style.pressed[2]};
    h["hideInGameMenus"] = g_style.hideInGameMenus;
    out["hud"] = std::move(h);
}

void Load(const nlohmann::json& in) {
    const Style defaults;
    g_style = defaults;
    if (!in.is_object() || !in.contains("hud") || !in["hud"].is_object()) {
        return;
    }
    const nlohmann::json& h = in["hud"];
    g_style.font = h.value("font", std::string("pixel")) == "luma" ? FontChoice::Luma : FontChoice::Pixel;
    g_style.background = std::clamp(h.value("background", defaults.background), 0.0f, 1.0f);
    g_style.textShadow = h.value("textShadow", defaults.textShadow);
    g_style.rounding = std::clamp(h.value("rounding", defaults.rounding), 0.0f, 8.0f);
    g_style.border = h.value("border", defaults.border);
    g_style.hideInGameMenus = h.value("hideInGameMenus", defaults.hideInGameMenus);
    auto color = [&](const char* key, float* out) {
        if (h.contains(key) && h[key].is_array() && h[key].size() == 3) {
            for (int i = 0; i < 3; ++i) {
                out[i] = std::clamp(h[key][i].get<float>(), 0.0f, 1.0f);
            }
        }
    };
    color("text", g_style.text);
    color("pressed", g_style.pressed);
}

}  // namespace luma::render::hud
