// =============================================================================
//  keys.h - Nombres de teclas
// =============================================================================
//  Windows identifica cada tecla con un "virtual key code" (VK): un número.
//  Por ejemplo 'L' = 0x4C, VK_END (Fin) = 0x23. Aquí los traducimos a un
//  nombre legible para mostrarlo en el menú.
//
//  En la configuración guardamos el número (no el nombre): así funciona igual
//  sin importar el idioma del teclado.
// =============================================================================
#pragma once

#include <string>

namespace luma::keys {

// 0 significa "sin tecla asignada".
constexpr int kNone = 0;

// Nombre de la tecla, por ejemplo "L", "F5", "Insert", "Fin".
std::string Name(int virtualKey);

// ¿Se puede asignar esta tecla? (Esc no: se usa para cancelar.)
bool IsBindable(int virtualKey);

}  // namespace luma::keys
