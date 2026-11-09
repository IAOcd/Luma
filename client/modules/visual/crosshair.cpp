// =============================================================================
//  crosshair.cpp - Implementación de la mira personalizada
// =============================================================================
#include "modules/visual/crosshair.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "render/hud.h"
#include "render/overlay.h"
#include "sdk/mouse_grab.h"

namespace luma::modules::visual {

Crosshair::Crosshair()
    : Module("crosshair", "Mira personalizada",
             "Dibuja tu propia mira en el centro. La del juego sigue debajo (para esconderla, usa un paquete de "
             "recursos; Luma podrá hacerlo en la Fase 8).",
             Category::Visual) {}

const char* Crosshair::IconSvg() const {
    return "M12 3v6 M12 15v6 M3 12h6 M15 12h6";
}

ImVec2 Crosshair::OnRenderHud(const HudFrame& frame) {
    // En los menús del juego (o con el de Luma abierto) no tiene sentido.
    if (hideInMenus_ && !frame.editing &&
        (render::overlay::IsMenuOpen() || sdk::mouse::GameMouseState() == 0)) {
        return ImVec2(0, 0);
    }
    ImDrawList* draw = frame.draw;
    const float u = render::hud::Measure(1.0f).px;  // un píxel de HUD (sigue la resolución)
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Centro exacto, redondeado a píxel (si no, las líneas finas se ven borrosas).
    const ImVec2 c(std::floor(display.x * 0.5f), std::floor(display.y * 0.5f));
    const float t = std::max(1.0f, std::round(thickness_ * u));
    const float len = std::round(length_ * u);
    const float gap = std::round(gap_ * u);
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(color_[0], color_[1], color_[2], color_[3]));
    const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(color_[3] * 200.0f));
    const float o = outline_ ? 1.0f : 0.0f;

    // Un rectángulo con su contorno (el contorno es el mismo, 1 px más grande).
    auto bar = [&](float x0, float y0, float x1, float y1) {
        if (outline_) {
            draw->AddRectFilled(ImVec2(x0 - o, y0 - o), ImVec2(x1 + o, y1 + o), outline);
        }
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color);
    };
    const float half = std::floor(t * 0.5f);
    if (style_ == 0 || style_ == 1) {
        bar(c.x - half, c.y - gap - len, c.x - half + t, c.y - gap);  // arriba
        bar(c.x - half, c.y + gap, c.x - half + t, c.y + gap + len);  // abajo
        bar(c.x - gap - len, c.y - half, c.x - gap, c.y - half + t);  // izquierda
        bar(c.x + gap, c.y - half, c.x + gap + len, c.y - half + t);  // derecha
    }
    if (style_ == 1 || style_ == 2) {
        bar(c.x - half, c.y - half, c.x - half + t, c.y - half + t);  // punto
    }
    if (style_ == 3) {
        const float radius = std::max(2.0f, gap + len * 0.5f);
        if (outline_) {
            draw->AddCircle(c, radius, outline, 32, t + 2.0f);
        }
        draw->AddCircle(c, radius, color, 32, t);
    }
    return ImVec2(0, 0);
}

bool Crosshair::DrawSettings() {
    bool changed = false;
    const char* styles[] = {"Cruz", "Cruz con punto", "Punto", "Círculo"};
    changed |= ImGui::Combo("Forma", &style_, styles, IM_ARRAYSIZE(styles));
    changed |= ImGui::SliderFloat("Largo", &length_, 1.0f, 15.0f, "%.0f");
    changed |= ImGui::SliderFloat("Grosor", &thickness_, 0.5f, 4.0f, "%.1f");
    changed |= ImGui::SliderFloat("Separación", &gap_, 0.0f, 10.0f, "%.0f");
    changed |= ImGui::ColorEdit4("Color", color_, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    changed |= ImGui::Checkbox("Contorno", &outline_);
    changed |= ImGui::Checkbox("Esconder en los menús del juego", &hideInMenus_);
    return changed;
}

void Crosshair::SaveSettings(nlohmann::json& out) const {
    out["style"] = style_;
    out["length"] = length_;
    out["thickness"] = thickness_;
    out["gap"] = gap_;
    out["color"] = {color_[0], color_[1], color_[2], color_[3]};
    out["outline"] = outline_;
    out["hideInMenus"] = hideInMenus_;
}

void Crosshair::LoadSettings(const nlohmann::json& in) {
    style_ = std::clamp(in.value("style", 0), 0, 3);
    length_ = std::clamp(in.value("length", 5.0f), 1.0f, 15.0f);
    thickness_ = std::clamp(in.value("thickness", 1.0f), 0.5f, 4.0f);
    gap_ = std::clamp(in.value("gap", 2.0f), 0.0f, 10.0f);
    if (in.contains("color") && in["color"].is_array() && in["color"].size() == 4) {
        for (int i = 0; i < 4; ++i) {
            color_[i] = std::clamp(in["color"][i].get<float>(), 0.0f, 1.0f);
        }
    }
    outline_ = in.value("outline", true);
    hideInMenus_ = in.value("hideInMenus", true);
}

}  // namespace luma::modules::visual
