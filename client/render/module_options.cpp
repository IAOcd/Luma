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
        ui::Section("Size and position");
        modules::HudLayout& layout = module.Layout();
        float percent = layout.scale * 100.0f;
        if (ui::Slider("Size", &percent, 50.0f, 400.0f, "%.0f%%")) {
            // Saltos de 25%: con la letra pixelada cada salto de 50% es nítido.
            layout.scale = std::round(percent / 25.0f) * 0.25f;
            changed = true;
        }
        if (ui::Button("Back to the default position", "Position")) {
            modules::Manager::ResetLayout(module);
            changed = true;
        }

        // --- Estilo propio ---------------------------------------------------------
        ui::Section("Style");
        bool& custom = module.CustomStyle();
        if (ui::Toggle("Own style", &custom, "(otherwise it uses Appearance)")) {
            if (custom) {
                module.ElementStyle() = hud::GetStyle();  // empezar desde el global
            }
            changed = true;
        }
        if (custom) {
            hud::Style& s = module.ElementStyle();
            int font = static_cast<int>(s.font);
            if (ui::Choice("Font", &font, {"Pixel", "Luma"})) {
                s.font = static_cast<hud::FontChoice>(font);
                changed = true;
            }
            changed |= ui::Slider("Background", &s.background, 0.0f, 1.0f, "%.2f");
            changed |= ui::Color("Background color", s.backgroundColor);
            changed |= ui::Color("Text color", s.text);
            changed |= ui::Color("Pressed color", s.pressed);
            changed |= ui::Toggle("Rainbow", &s.rainbow, "(the text keeps changing color)");
            if (s.rainbow) changed |= ui::Slider("Rainbow speed", &s.rainbowSpeed, 0.05f, 2.0f, "%.2f");
            changed |= ui::Toggle("Text shadow", &s.textShadow);
            changed |= ui::Toggle("Border", &s.border);
            changed |= ui::Slider("Corners", &s.rounding, 0.0f, 6.0f, "%.0f");
            if (ui::Button("Copy the Appearance style", "Reset")) {
                s = hud::GetStyle();
                changed = true;
            }
        }
    }
    return changed;
}

}  // namespace luma::render::moduleoptions
