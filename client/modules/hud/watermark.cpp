// =============================================================================
//  watermark.cpp - Implementación de la marca de agua
// =============================================================================
#include "modules/hud/watermark.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include "core/version.h"
#include "render/assets.h"
#include "render/icons.h"
#include "render/theme.h"

namespace luma::modules::hud {

Watermark::Watermark()
    : Module("watermark", "Marca de agua", "Muestra \"Luma\" en una esquina de la pantalla.",
             Category::Hud) {}

const char* Watermark::IconSvg() const {
    return render::icons::kLumaLogo;
}

void Watermark::OnRenderHud() {
    // GetForegroundDrawList = una "capa" que ImGui dibuja encima de todo.
    // Dibujar directamente ahí es lo más barato: sin ventanas ni layout.
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const theme::Fonts& fonts = theme::GetFonts();
    ImFont* font = fonts.title != nullptr ? fonts.title : ImGui::GetFont();
    const float size = ImGui::GetFontSize() * scale_;
    const float margin = size * 0.6f;
    const float logoSize = size * 1.05f;
    const float gap = size * 0.35f;

    const char* name = kName;
    char version[16] = "";
    if (showVersion_) {
        // snprintf escribe en un arreglo fijo: sin pedir memoria cada frame.
        std::snprintf(version, sizeof(version), " %s", kVersion);
    }
    const ImVec2 nameSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name);
    const ImVec2 versionSize = ImGui::GetFont()->CalcTextSizeA(size * 0.55f, FLT_MAX, 0.0f, version);
    const float width = logoSize + gap + nameSize.x + versionSize.x;
    const float height = std::max(logoSize, nameSize.y);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const bool right = corner_ == 1 || corner_ == 3;
    const bool bottom = corner_ == 2 || corner_ == 3;
    const ImVec2 pos(right ? display.x - width - margin : margin,
                     bottom ? display.y - height - margin : margin);

    // Marfil de Luma con una sombra suave para que se lea sobre cualquier fondo.
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(theme::kIvory);
    const ImU32 shadow = IM_COL32(0, 0, 0, 120);
    const ImVec2 logoCenter(pos.x + logoSize * 0.5f, pos.y + height * 0.5f);
    render::assets::DrawLogo(draw, ImVec2(logoCenter.x + 1.5f, logoCenter.y + 1.5f), logoSize, shadow);
    render::assets::DrawLogo(draw, logoCenter, logoSize, color);

    const ImVec2 textPos(pos.x + logoSize + gap, pos.y + (height - nameSize.y) * 0.5f);
    draw->AddText(font, size, ImVec2(textPos.x + 1.5f, textPos.y + 1.5f), shadow, name);
    draw->AddText(font, size, textPos, color, name);
    if (showVersion_) {
        const ImVec2 vpos(textPos.x + nameSize.x, textPos.y + nameSize.y - versionSize.y - size * 0.1f);
        draw->AddText(ImGui::GetFont(), size * 0.55f, vpos, IM_COL32(233, 230, 223, 170), version);
    }
}

bool Watermark::DrawSettings() {
    bool changed = false;
    const char* corners[] = {"Arriba a la izquierda", "Arriba a la derecha", "Abajo a la izquierda",
                             "Abajo a la derecha"};
    changed |= ImGui::Combo("Esquina", &corner_, corners, IM_ARRAYSIZE(corners));
    changed |= ImGui::Checkbox("Mostrar versión", &showVersion_);
    changed |= ImGui::SliderFloat("Tamaño", &scale_, 0.8f, 3.0f, "%.1fx");
    return changed;
}

void Watermark::SaveSettings(nlohmann::json& out) const {
    out["corner"] = corner_;
    out["showVersion"] = showVersion_;
    out["scale"] = scale_;
}

void Watermark::LoadSettings(const nlohmann::json& in) {
    corner_ = std::clamp(in.value("corner", 0), 0, 3);
    showVersion_ = in.value("showVersion", true);
    scale_ = std::clamp(in.value("scale", 1.3f), 0.8f, 3.0f);
}

}  // namespace luma::modules::hud
