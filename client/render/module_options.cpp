// =============================================================================
//  module_options.cpp - Implementación de las opciones de un módulo
// =============================================================================
#include "render/module_options.h"

#include <imgui.h>

#include <cmath>

#include "modules/module_manager.h"
#include "render/hud.h"
#include "render/ui.h"

namespace luma::render::moduleoptions {

bool Draw(modules::Module& module) {
    bool changed = false;

    // --- Las opciones propias del módulo (lo más importante, arriba) -----------
    if (module.HasSettings()) {
        changed |= module.DrawSettings();
    }

    if (module.IsHudElement()) {
        // --- Posición y tamaño ---------------------------------------------------
        ui::Section("Tamaño y posición");
        modules::HudLayout& layout = module.Layout();
        float percent = layout.scale * 100.0f;
        if (ui::Slider("Tamaño", &percent, 50.0f, 400.0f, "%.0f%%")) {
            // Saltos de 25%: con la letra pixelada cada salto de 50% es nítido.
            layout.scale = std::round(percent / 25.0f) * 0.25f;
            changed = true;
        }
        if (ui::Button("Volver a la posición de fábrica", "Posición")) {
            modules::Manager::ResetLayout(module);
            changed = true;
        }

        // --- Estilo propio ---------------------------------------------------------
        ui::Section("Estilo");
        bool& custom = module.CustomStyle();
        if (ui::Toggle("Estilo propio", &custom, "(si no, usa el de Tema)")) {
            if (custom) {
                module.ElementStyle() = hud::GetStyle();  // empezar desde el global
            }
            changed = true;
        }
        if (custom) {
            hud::Style& s = module.ElementStyle();
            int font = static_cast<int>(s.font);
            if (ui::Choice("Letra", &font, {"Pixel", "Luma"})) {
                s.font = static_cast<hud::FontChoice>(font);
                changed = true;
            }
            changed |= ui::Slider("Fondo", &s.background, 0.0f, 1.0f, "%.2f");
            changed |= ui::Color("Color del fondo", s.backgroundColor);
            changed |= ui::Color("Color del texto", s.text);
            changed |= ui::Color("Color al apretar", s.pressed);
            changed |= ui::Toggle("Sombra del texto", &s.textShadow);
            changed |= ui::Toggle("Borde", &s.border);
            changed |= ui::Slider("Esquinas", &s.rounding, 0.0f, 6.0f, "%.0f");
            if (ui::Button("Copiar el estilo de Tema", "Restablecer")) {
                s = hud::GetStyle();
                changed = true;
            }
        }
    }
    return changed;
}

}  // namespace luma::render::moduleoptions
