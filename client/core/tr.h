// =============================================================================
//  tr.h - Traducir los textos del juego escritos en español
// =============================================================================
//  El launcher usa claves ("home.play" -> "PLAY"), ver shared/i18n.h. En la DLL
//  hay cientos de textos repartidos por los módulos ("Mostrar el nombre",
//  "Color del texto"...), y ponerle una clave a cada uno haría el código
//  difícil de leer. Por eso aquí la "clave" ES el texto en español:
//
//      ui::Toggle(Tr("Mostrar el nombre"), &showName_);
//
//  Tr() busca ese texto en la tabla (core/tr_table.inc, una línea por texto
//  con español, inglés y portugués) y devuelve el del idioma elegido. Si no
//  está en la tabla, devuelve el mismo texto en español (así nunca se rompe
//  nada: a lo sumo se ve un texto sin traducir).
//
//  Los controles de render/ui.h ya llaman a Tr() solos, así que las opciones
//  de los módulos se traducen sin tocar cada módulo.
//
//  Costo: una búsqueda en una tabla hash (texto corto). Solo con el menú
//  abierto o para unos pocos textos del HUD: nada en el hot path.
// =============================================================================
#pragma once

#include <format>
#include <string>

namespace luma {

const char* Tr(const char* spanish);

// Igual que std::format, pero traduciendo primero el formato:
//     TrFormat("Perfil creado: {}", name)
template <typename... Args>
std::string TrFormat(const char* spanishFormat, Args&&... args) {
    return std::vformat(Tr(spanishFormat), std::make_format_args(args...));
}

}  // namespace luma
