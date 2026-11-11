// =============================================================================
//  crosshair.cpp - La mira personalizada (ver crosshair.h)
// =============================================================================
#include "modules/visual/crosshair.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "core/tr.h"
#include "render/hud.h"
#include "render/overlay.h"
#include "render/rainbow.h"
#include "render/ui.h"
#include "sdk/game_crosshair.h"
#include "sdk/mouse_grab.h"

namespace luma::modules::visual {

namespace {

// La paleta del editor: blancos y grises, y los colores de Minecraft.
constexpr std::uint32_t kPalette[] = {
    IM_COL32(255, 255, 255, 255), IM_COL32(190, 190, 190, 255), IM_COL32(110, 110, 110, 255), IM_COL32(0, 0, 0, 255),
    IM_COL32(255, 85, 85, 255),   IM_COL32(255, 170, 0, 255),   IM_COL32(255, 255, 85, 255),  IM_COL32(85, 255, 85, 255),
    IM_COL32(0, 170, 0, 255),     IM_COL32(85, 255, 255, 255),  IM_COL32(0, 170, 170, 255),   IM_COL32(85, 85, 255, 255),
    IM_COL32(0, 0, 170, 255),     IM_COL32(255, 85, 255, 255),  IM_COL32(170, 0, 170, 255),   IM_COL32(242, 182, 90, 255),
};

std::uint32_t Pack(const float* c) {
    return IM_COL32(static_cast<int>(c[0] * 255.0f + 0.5f), static_cast<int>(c[1] * 255.0f + 0.5f),
                    static_cast<int>(c[2] * 255.0f + 0.5f), static_cast<int>(c[3] * 255.0f + 0.5f));
}

void Unpack(std::uint32_t v, float* c) {
    const ImVec4 f = ImGui::ColorConvertU32ToFloat4(v);
    c[0] = f.x;
    c[1] = f.y;
    c[2] = f.z;
    c[3] = f.w;
}

bool ValidSize(int n) {
    return n == 8 || n == 16 || n == 32 || n == 64;
}

}  // namespace

Crosshair::Crosshair()
    : Module("crosshair", "Mira personalizada",
             "Tu propia mira en el centro: una forma o una que pintas píxel por píxel, con colores o rainbow. Esconde la "
             "mira del juego.",
             Category::Visual) {
    Resize(gridSize_);
    ShapeToPixels();
}

const char* Crosshair::IconSvg() const {
    return "M12 3v6 M12 15v6 M3 12h6 M15 12h6";
}

void Crosshair::OnEnable() {
    sdk::gamecrosshair::SetHidden(hideGame_);
}

void Crosshair::OnDisable() {
    sdk::gamecrosshair::SetHidden(false);  // la del juego vuelve
}

// El color final de un píxel: el original o el del arcoíris (con su alfa).
std::uint32_t Crosshair::Tinted(std::uint32_t color, float offset) const {
    if (colorMode_ == 0) return color;
    float r = 1, g = 1, b = 1;
    rainbow::Color(rainbowSpeed_, colorMode_ == 2 ? offset : 0.0f, r, g, b);
    const std::uint32_t alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f), static_cast<int>(b * 255.0f), alpha);
}

ImVec2 Crosshair::OnRenderHud(const HudFrame& frame) {
    sdk::gamecrosshair::SetHidden(hideGame_);  // (barato: solo guarda un sí/no)
    // En los menús del juego (o con el de Luma abierto) no tiene sentido.
    if (hideInMenus_ && !frame.editing && (render::overlay::IsMenuOpen() || sdk::mouse::GameMouseState() == 0)) {
        return ImVec2(0, 0);
    }
    const float u = render::hud::Measure(1.0f).px;  // un píxel de HUD (sigue la resolución)
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Centro exacto, redondeado a píxel (si no, las líneas finas se ven borrosas).
    const ImVec2 c(std::floor(display.x * 0.5f), std::floor(display.y * 0.5f));
    if (mode_ == 1) {
        DrawPainted(frame.draw, c, u);
    } else {
        DrawShape(frame.draw, c, u);
    }
    return ImVec2(0, 0);
}

void Crosshair::DrawShape(ImDrawList* draw, ImVec2 c, float u) const {
    const float t = std::max(1.0f, std::round(thickness_ * u));
    const float len = std::round(length_ * u);
    const float gap = std::round(gap_ * u);
    const std::uint32_t base = Pack(color_);
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(color_[3] * 200.0f));
    const float o = outline_ ? 1.0f : 0.0f;
    int index = 0;
    // Un rectángulo con su contorno (el contorno es el mismo, 1 px más grande).
    auto bar = [&](float x0, float y0, float x1, float y1) {
        if (outline_) draw->AddRectFilled(ImVec2(x0 - o, y0 - o), ImVec2(x1 + o, y1 + o), outline);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Tinted(base, 0.25f * static_cast<float>(index++)));
    };
    const float half = std::floor(t * 0.5f);
    if (style_ == 0 || style_ == 1) {
        bar(c.x - half, c.y - gap - len, c.x - half + t, c.y - gap);  // arriba
        bar(c.x + gap, c.y - half, c.x + gap + len, c.y - half + t);  // derecha
        bar(c.x - half, c.y + gap, c.x - half + t, c.y + gap + len);  // abajo
        bar(c.x - gap - len, c.y - half, c.x - gap, c.y - half + t);  // izquierda
    }
    if (style_ == 1 || style_ == 2) {
        bar(c.x - half, c.y - half, c.x - half + t, c.y - half + t);  // punto
    }
    if (style_ == 3) {
        const float radius = std::max(2.0f, gap + len * 0.5f);
        if (outline_) draw->AddCircle(c, radius, outline, 32, t + 2.0f);
        draw->AddCircle(c, radius, Tinted(base, 0.0f), 32, t);
    }
}

void Crosshair::DrawPainted(ImDrawList* draw, ImVec2 c, float u) const {
    const float cell = std::max(1.0f, std::round(pixelScale_ * u));
    const float side = cell * static_cast<float>(gridSize_);
    const ImVec2 origin(std::floor(c.x - side * 0.5f), std::floor(c.y - side * 0.5f));
    if (colorMode_ == 2) {
        // Onda: cada píxel con su tono (no se pueden juntar en tiras).
        const float n = static_cast<float>(gridSize_);
        for (const Run& r : runs_) {
            for (int x = r.x0; x < r.x1; ++x) {
                const float offset = (static_cast<float>(x) + static_cast<float>(r.y)) / (2.0f * n);
                const ImVec2 p(origin.x + x * cell, origin.y + r.y * cell);
                draw->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), Tinted(r.color, offset));
            }
        }
        return;
    }
    for (const Run& r : runs_) {
        const ImVec2 p(origin.x + r.x0 * cell, origin.y + r.y * cell);
        draw->AddRectFilled(p, ImVec2(origin.x + r.x1 * cell, p.y + cell), Tinted(r.color, 0.0f));
    }
}

// Las tiras de píxeles iguales seguidos (para dibujar barato).
void Crosshair::RebuildRuns() {
    runs_.clear();
    const int n = gridSize_;
    for (int y = 0; y < n; ++y) {
        int x = 0;
        while (x < n) {
            const std::uint32_t c = pixels_[static_cast<std::size_t>(y * n + x)];
            if (((c >> IM_COL32_A_SHIFT) & 0xFF) == 0) {
                ++x;
                continue;
            }
            int end = x + 1;
            while (end < n && pixels_[static_cast<std::size_t>(y * n + end)] == c) ++end;
            runs_.push_back({static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(end), c});
            x = end;
        }
    }
}

// Cambiar el tamaño de la cuadrícula conservando el dibujo (agrandado o achicado).
void Crosshair::Resize(int size) {
    if (!ValidSize(size)) size = 16;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(size) * size, 0);
    if (!pixels_.empty() && ValidSize(gridSize_) && pixels_.size() == static_cast<std::size_t>(gridSize_) * gridSize_) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int sx = x * gridSize_ / size;
                const int sy = y * gridSize_ / size;
                out[static_cast<std::size_t>(y * size + x)] = pixels_[static_cast<std::size_t>(sy * gridSize_ + sx)];
            }
        }
    }
    gridSize_ = size;
    pixels_ = std::move(out);
    RebuildRuns();
}

// Pasar la forma elegida a píxeles (para empezar a pintar desde ella).
void Crosshair::ShapeToPixels() {
    const int n = gridSize_;
    std::fill(pixels_.begin(), pixels_.end(), 0u);
    const std::uint32_t c = Pack(color_);
    const float scale = static_cast<float>(n) / 16.0f;  // la forma está pensada para 16x16
    const int t = std::max(1, static_cast<int>(std::round(thickness_ * scale)));
    const int len = std::max(1, static_cast<int>(std::round(length_ * scale)));
    const int gap = static_cast<int>(std::round(gap_ * scale));
    const int c0 = n / 2 - t / 2;  // la columna/fila central
    auto set = [&](int x, int y) {
        if (x >= 0 && y >= 0 && x < n && y < n) pixels_[static_cast<std::size_t>(y * n + x)] = c;
    };
    if (style_ == 0 || style_ == 1) {
        for (int k = 0; k < t; ++k) {
            for (int i = 0; i < len; ++i) {
                set(c0 + k, c0 - gap - 1 - i);      // arriba
                set(c0 + k, c0 + t + gap + i);      // abajo
                set(c0 - gap - 1 - i, c0 + k);      // izquierda
                set(c0 + t + gap + i, c0 + k);      // derecha
            }
        }
    }
    if (style_ == 1 || style_ == 2) {
        for (int y = 0; y < t; ++y) {
            for (int x = 0; x < t; ++x) set(c0 + x, c0 + y);
        }
    }
    if (style_ == 3) {
        const float r = std::max(2.0f, static_cast<float>(gap) + static_cast<float>(len) * 0.5f);
        const float cc = static_cast<float>(n) * 0.5f - 0.5f;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const float d = std::sqrt((x - cc) * (x - cc) + (y - cc) * (y - cc));
                if (std::fabs(d - r) <= static_cast<float>(t) * 0.5f) set(x, y);
            }
        }
    }
    RebuildRuns();
}

// El editor de píxeles (una ventana grande encima del menú).
bool Crosshair::DrawEditor() {
    bool changed = false;
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 46.0f, fs * 33.0f), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(Tr("Editor de mira"), &open, ImGuiWindowFlags_NoSavedSettings)) {
        return false;
    }
    const int n = gridSize_;
    // --- La cuadrícula (a la izquierda) ---
    const float avail = std::min(ImGui::GetContentRegionAvail().y - fs * 0.5f, fs * 29.0f);
    const float cell = std::floor(avail / static_cast<float>(n));
    const float side = cell * static_cast<float>(n);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##canvas", ImVec2(side, side), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* d = ImGui::GetWindowDrawList();
    // fondo a cuadros (para ver lo transparente)
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const ImVec2 a(p0.x + x * cell, p0.y + y * cell);
            const bool dark = ((x + y) & 1) != 0;
            d->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), dark ? IM_COL32(52, 54, 58, 255) : IM_COL32(64, 66, 70, 255));
            const std::uint32_t c = pixels_[static_cast<std::size_t>(y * n + x)];
            if (((c >> IM_COL32_A_SHIFT) & 0xFF) != 0) d->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), c);
        }
    }
    // líneas de la cuadrícula (si los cuadros son grandes) y el centro
    if (cell >= 6.0f) {
        for (int i = 0; i <= n; ++i) {
            const ImU32 lc = i == n / 2 ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 14);
            d->AddLine(ImVec2(p0.x + i * cell, p0.y), ImVec2(p0.x + i * cell, p0.y + side), lc);
            d->AddLine(ImVec2(p0.x, p0.y + i * cell), ImVec2(p0.x + side, p0.y + i * cell), lc);
        }
    }
    d->AddRect(p0, ImVec2(p0.x + side, p0.y + side), IM_COL32(255, 255, 255, 60));
    // pintar (izquierdo) o borrar (derecho), con espejo
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int x = static_cast<int>((m.x - p0.x) / cell);
        const int y = static_cast<int>((m.y - p0.y) / cell);
        const bool paint = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool erase = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (x >= 0 && y >= 0 && x < n && y < n) {
            const ImVec2 a(p0.x + x * cell, p0.y + y * cell);
            d->AddRect(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(255, 255, 255, 200));
            if (paint || erase) {
                const std::uint32_t value = erase ? 0u : Pack(brush_);
                auto put = [&](int px, int py) {
                    std::uint32_t& dst = pixels_[static_cast<std::size_t>(py * n + px)];
                    if (dst != value) {
                        dst = value;
                        changed = true;
                    }
                };
                put(x, y);
                if (mirrorX_) put(n - 1 - x, y);
                if (mirrorY_) put(x, n - 1 - y);
                if (mirrorX_ && mirrorY_) put(n - 1 - x, n - 1 - y);
            }
        }
    }
    // --- Herramientas (a la derecha) ---
    ImGui::SameLine(0.0f, fs);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(Tr("Color del pincel"));
    for (int i = 0; i < static_cast<int>(std::size(kPalette)); ++i) {
        if (i % 4 != 0) ImGui::SameLine(0.0f, fs * 0.3f);
        ImGui::PushID(i);
        float c[4];
        Unpack(kPalette[i], c);
        if (ImGui::ColorButton("##p", ImVec4(c[0], c[1], c[2], c[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(fs * 2.0f, fs * 2.0f))) {
            std::copy(c, c + 4, brush_);
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, fs * 0.3f));
    ImGui::ColorEdit4("##brush", brush_, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("(otro color)"));
    ImGui::Dummy(ImVec2(0, fs * 0.5f));
    ImGui::Checkbox(Tr("Espejo izquierda / derecha"), &mirrorX_);
    ImGui::Checkbox(Tr("Espejo arriba / abajo"), &mirrorY_);
    ImGui::Dummy(ImVec2(0, fs * 0.5f));
    ImGui::TextDisabled("%s", Tr("Clic izquierdo: pintar · Clic derecho: borrar"));
    ImGui::Dummy(ImVec2(0, fs * 0.5f));
    if (ImGui::Button(Tr("Limpiar"))) {
        std::fill(pixels_.begin(), pixels_.end(), 0u);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Empezar desde la forma"))) {
        ShapeToPixels();
        changed = true;
    }
    // vista previa a tamaño real
    ImGui::Dummy(ImVec2(0, fs * 0.5f));
    ImGui::TextUnformatted(Tr("Así se ve:"));
    const ImVec2 pv = ImGui::GetCursorScreenPos();
    const float pvSide = fs * 6.0f;
    d->AddRectFilled(pv, ImVec2(pv.x + pvSide, pv.y + pvSide), IM_COL32(110, 150, 90, 255), 4.0f);  // "pasto"
    if (changed) RebuildRuns();
    DrawPainted(d, ImVec2(std::floor(pv.x + pvSide * 0.5f), std::floor(pv.y + pvSide * 0.5f)), render::hud::Measure(1.0f).px);
    ImGui::Dummy(ImVec2(pvSide, pvSide));
    ImGui::Dummy(ImVec2(0, fs * 0.5f));
    if (ImGui::Button(Tr("Listo"), ImVec2(fs * 8.0f, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndGroup();
    ImGui::EndPopup();
    if (changed) RebuildRuns();
    return changed;
}

bool Crosshair::DrawSettings() {
    namespace ui = render::ui;
    bool changed = false;
    if (ui::Toggle("Esconder la mira del juego", &hideGame_)) {
        if (IsEnabled()) sdk::gamecrosshair::SetHidden(hideGame_);
        changed = true;
    }
    if (!sdk::gamecrosshair::Available()) ui::Hint("No se encontró la mira del juego en esta versión: queda visible debajo.");
    changed |= ui::Choice("Tipo", &mode_, {"Forma", "Pintada"});
    if (mode_ == 0) {
        changed |= ui::Choice("Forma", &style_, {"Cruz", "Cruz con punto", "Punto", "Círculo"});
        changed |= ui::Slider("Largo", &length_, 1.0f, 15.0f, "%.0f");
        changed |= ui::Slider("Grosor", &thickness_, 0.5f, 4.0f, "%.1f");
        changed |= ui::Slider("Separación", &gap_, 0.0f, 10.0f, "%.0f");
        changed |= ui::ColorAlpha("Color", color_);
        changed |= ui::Toggle("Contorno", &outline_, "(se ve sobre fondos claros)");
    } else {
        int sizeIndex = gridSize_ == 8 ? 0 : gridSize_ == 16 ? 1 : gridSize_ == 32 ? 2 : 3;
        if (ui::Choice("Cuadrícula", &sizeIndex, {"8x8", "16x16", "32x32", "64x64"})) {
            static const int kSizes[] = {8, 16, 32, 64};
            Resize(kSizes[sizeIndex]);
            changed = true;
        }
        changed |= ui::Slider("Tamaño del píxel", &pixelScale_, 0.25f, 3.0f, "%.2f");
        if (ui::Button("Pintar mi mira", "Editor")) ImGui::OpenPopup(Tr("Editor de mira"));
        changed |= DrawEditor();
    }
    ui::Section("Color");
    changed |= ui::Choice("Modo de color", &colorMode_, {"Original", "Rainbow", "Rainbow en onda"});
    if (colorMode_ != 0) changed |= ui::Slider("Velocidad del rainbow", &rainbowSpeed_, 0.05f, 2.0f, "%.2f");
    changed |= ui::Toggle("Esconder en los menús", &hideInMenus_);
    return changed;
}

void Crosshair::SaveSettings(nlohmann::json& out) const {
    out["mode"] = mode_;
    out["style"] = style_;
    out["length"] = length_;
    out["thickness"] = thickness_;
    out["gap"] = gap_;
    out["color"] = {color_[0], color_[1], color_[2], color_[3]};
    out["outline"] = outline_;
    out["colorMode"] = colorMode_;
    out["rainbowSpeed"] = rainbowSpeed_;
    out["gridSize"] = gridSize_;
    out["pixelScale"] = pixelScale_;
    out["hideGame"] = hideGame_;
    out["hideInMenus"] = hideInMenus_;
    out["mirrorX"] = mirrorX_;
    out["mirrorY"] = mirrorY_;
    // Los píxeles como texto: 8 dígitos hexadecimales por píxel (RRGGBBAA).
    std::string hex;
    hex.reserve(pixels_.size() * 8);
    char buf[9];
    for (std::uint32_t c : pixels_) {
        const unsigned r = (c >> IM_COL32_R_SHIFT) & 0xFF, g = (c >> IM_COL32_G_SHIFT) & 0xFF, b = (c >> IM_COL32_B_SHIFT) & 0xFF,
                       a = (c >> IM_COL32_A_SHIFT) & 0xFF;
        std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", r, g, b, a);
        hex += buf;
    }
    out["pixels"] = hex;
}

void Crosshair::LoadSettings(const nlohmann::json& in) {
    mode_ = std::clamp(in.value("mode", 0), 0, 1);
    style_ = std::clamp(in.value("style", 0), 0, 3);
    length_ = std::clamp(in.value("length", 5.0f), 1.0f, 15.0f);
    thickness_ = std::clamp(in.value("thickness", 1.0f), 0.5f, 4.0f);
    gap_ = std::clamp(in.value("gap", 2.0f), 0.0f, 10.0f);
    if (in.contains("color") && in["color"].is_array() && in["color"].size() == 4) {
        for (int i = 0; i < 4; ++i) color_[i] = std::clamp(in["color"][i].get<float>(), 0.0f, 1.0f);
    }
    outline_ = in.value("outline", true);
    colorMode_ = std::clamp(in.value("colorMode", 0), 0, 2);
    rainbowSpeed_ = std::clamp(in.value("rainbowSpeed", 0.25f), 0.05f, 2.0f);
    pixelScale_ = std::clamp(in.value("pixelScale", 1.0f), 0.25f, 3.0f);
    hideGame_ = in.value("hideGame", true);
    hideInMenus_ = in.value("hideInMenus", true);
    mirrorX_ = in.value("mirrorX", true);
    mirrorY_ = in.value("mirrorY", true);
    const int size = in.value("gridSize", 16);
    gridSize_ = ValidSize(size) ? size : 16;
    pixels_.assign(static_cast<std::size_t>(gridSize_) * gridSize_, 0u);
    const std::string hex = in.value("pixels", std::string());
    if (hex.size() == pixels_.size() * 8) {
        for (std::size_t i = 0; i < pixels_.size(); ++i) {
            const std::string part = hex.substr(i * 8, 8);
            const unsigned long v = std::strtoul(part.c_str(), nullptr, 16);
            pixels_[i] = IM_COL32((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        }
        RebuildRuns();
    } else {
        ShapeToPixels();  // (sin dibujo guardado: empezar desde la forma)
    }
}

}  // namespace luma::modules::visual
