// =============================================================================
//  watermark.cpp - Implementación de la marca de agua
// =============================================================================
#include "modules/hud/watermark.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include "core/version.h"
#include "render/theme.h"

namespace luma::modules::hud {

Watermark::Watermark()
    : Module("watermark", "Marca de agua", "Muestra \"Luma\" en una esquina de la pantalla.",
             Category::Hud) {}

void Watermark::OnRenderHud() {
    // GetForegroundDrawList = una "capa" que ImGui dibuja encima de todo.
    // Dibujar directamente ahí es lo más barato: sin ventanas ni layout.
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * scale_;
    const float margin = size * 0.6f;

    const char* name = kName;
    char version[16] = "";
    if (showVersion_) {
        // snprintf escribe en un arreglo fijo: sin pedir memoria cada frame.
        std::snprintf(version, sizeof(version), " %s", kVersion);
    }
    const ImVec2 nameSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name);
    const ImVec2 versionSize = font->CalcTextSizeA(size * 0.6f, FLT_MAX, 0.0f, version);
    const float width = nameSize.x + versionSize.x;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const bool right = corner_ == 1 || corner_ == 3;
    const bool bottom = corner_ == 2 || corner_ == 3;
    const ImVec2 pos(right ? display.x - width - margin : margin,
                     bottom ? display.y - nameSize.y - margin : margin);

    const ImU32 accent = theme::AccentU32(1.0f);
    const ImU32 shadow = IM_COL32(0, 0, 0, 140);
    // Sombra (el mismo texto, corrido 2 píxeles) para que se lea sobre fondos claros.
    draw->AddText(font, size, ImVec2(pos.x + 2, pos.y + 2), shadow, name);
    draw->AddText(font, size, pos, accent, name);
    if (showVersion_) {
        const ImVec2 vpos(pos.x + nameSize.x, pos.y + nameSize.y - versionSize.y - size * 0.08f);
        draw->AddText(font, size * 0.6f, vpos, IM_COL32(230, 230, 230, 200), version);
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
