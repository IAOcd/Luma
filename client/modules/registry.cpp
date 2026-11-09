// =============================================================================
//  registry.cpp - La lista de todos los módulos de Luma
// =============================================================================
//  PARA AGREGAR UN MÓDULO NUEVO:
//    1. Crea su archivo en la carpeta de su categoría (por ejemplo
//       modules/hud/mi_modulo.h y .cpp) con una clase que herede de Module.
//    2. Agrégalo a la lista de abajo.
//    3. Agrega el .cpp en client/CMakeLists.txt.
//  El menú, las teclas, la configuración y el editor de HUD funcionan solos.
//
//  El orden de la lista es el orden en el menú (dentro de cada categoría) y
//  el orden de dibujo (el último queda encima).
// =============================================================================
#include "modules/module_manager.h"

#include "modules/hud/hud_modules.h"
#include "modules/hud/watermark.h"
#include "modules/performance/perf_overlay.h"
#include "modules/performance/process_opts.h"
#include "modules/utility/toggle_keys.h"
#include "modules/visual/crosshair.h"

namespace luma::modules {

void Manager::RegisterAll() {
    modules_.clear();
    // HUD
    modules_.push_back(std::make_unique<hud::Keystrokes>());
    modules_.push_back(std::make_unique<hud::FpsCounter>());
    modules_.push_back(std::make_unique<hud::CpsCounter>());
    modules_.push_back(std::make_unique<hud::Clock>());
    modules_.push_back(std::make_unique<hud::Watermark>());
    // Visual
    modules_.push_back(std::make_unique<visual::Crosshair>());
    // Utilidad
    modules_.push_back(std::make_unique<utility::ToggleKey>(render::input::HoldSlot::Sprint));
    modules_.push_back(std::make_unique<utility::ToggleKey>(render::input::HoldSlot::Sneak));
    // Rendimiento
    modules_.push_back(std::make_unique<performance::PerfOverlay>());
    modules_.push_back(std::make_unique<performance::ProcessPriority>());
    modules_.push_back(std::make_unique<performance::TimerResolution>());
    modules_.push_back(std::make_unique<performance::NoEcoQos>());
    modules_.push_back(std::make_unique<performance::RenderThreadPriority>());

    // Reservar espacio para la lista de activos de una vez: así activar o
    // desactivar módulos nunca pide memoria nueva.
    activeHud_.reserve(modules_.size());
}

}  // namespace luma::modules
