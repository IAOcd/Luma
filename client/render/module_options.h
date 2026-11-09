// =============================================================================
//  module_options.h - Las opciones de un módulo (menú y editor de HUD)
// =============================================================================
//  Lo mismo se muestra en dos lugares: la página del módulo en el menú y el
//  panel que se abre con clic derecho en el editor de HUD. Tiene:
//
//    - Tamaño y "volver a la posición de fábrica" (si es un elemento del HUD).
//    - Estilo propio: fondo, colores, sombra, borde, esquinas y letra, solo
//      para este elemento (si no, usa el estilo global de Tema).
//    - Las opciones propias del módulo (Module::DrawSettings).
//
//  HILOS: dentro de un frame de ImGui, con luma::StateMutex() tomado.
// =============================================================================
#pragma once

namespace luma::modules {
class Module;
}

namespace luma::render::moduleoptions {

// Dibuja todas las opciones. Devuelve true si algo cambió (para guardar).
bool Draw(modules::Module& module);

}  // namespace luma::render::moduleoptions
