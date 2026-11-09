// =============================================================================
//  registry.cpp - La lista de todos los módulos de Luma
// =============================================================================
//  PARA AGREGAR UN MÓDULO NUEVO:
//    1. Crea su archivo en la carpeta de su categoría (por ejemplo
//       modules/hud/mi_modulo.h y .cpp) con una clase que herede de Module.
//    2. Agrégalo a la lista de abajo.
//    3. Agrega el .cpp en client/CMakeLists.txt.
//  El menú, las teclas y la configuración funcionan solos.
// =============================================================================
#include "modules/module_manager.h"

#include "modules/hud/watermark.h"
#include "modules/performance/perf_overlay.h"

namespace luma::modules {

void Manager::RegisterAll() {
    modules_.clear();
    modules_.push_back(std::make_unique<hud::Watermark>());
    modules_.push_back(std::make_unique<performance::PerfOverlay>());

    // Reservar espacio para la lista de activos de una vez: así activar o
    // desactivar módulos nunca pide memoria nueva.
    activeHud_.reserve(modules_.size());
}

}  // namespace luma::modules
