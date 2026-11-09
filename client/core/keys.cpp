// =============================================================================
//  keys.cpp - Nombres de teclas
// =============================================================================
#include "core/keys.h"

#include <windows.h>

#include <format>

namespace luma::keys {

std::string Name(int vk) {
    if (vk == kNone) {
        return "Ninguna";
    }
    // Letras y números: su código coincide con el carácter ASCII.
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return std::format("F{}", vk - VK_F1 + 1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return std::format("Num {}", vk - VK_NUMPAD0);
    }
    switch (vk) {
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Supr";
        case VK_HOME:     return "Inicio";
        case VK_END:      return "Fin";
        case VK_PRIOR:    return "RePág";
        case VK_NEXT:     return "AvPág";
        case VK_UP:       return "Flecha arriba";
        case VK_DOWN:     return "Flecha abajo";
        case VK_LEFT:     return "Flecha izquierda";
        case VK_RIGHT:    return "Flecha derecha";
        case VK_SPACE:    return "Espacio";
        case VK_TAB:      return "Tab";
        case VK_RETURN:   return "Enter";
        case VK_BACK:     return "Retroceso";
        case VK_SHIFT:    return "Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_MENU:     return "Alt";
        case VK_CAPITAL:  return "Bloq Mayús";
        case VK_PAUSE:    return "Pausa";
        case VK_SCROLL:   return "Bloq Despl";
        case VK_MULTIPLY: return "Num *";
        case VK_ADD:      return "Num +";
        case VK_SUBTRACT: return "Num -";
        case VK_DECIMAL:  return "Num .";
        case VK_DIVIDE:   return "Num /";
        case VK_OEM_3:    return "Tecla ` / ñ";
        case VK_OEM_MINUS: return "Tecla -";
        case VK_OEM_PLUS: return "Tecla +";
        case VK_OEM_COMMA: return "Tecla ,";
        case VK_OEM_PERIOD: return "Tecla .";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_MBUTTON:  return "Clic central";
        default:          return std::format("Tecla 0x{:02X}", vk);
    }
}

bool IsBindable(int vk) {
    return vk > 0 && vk < 256 && vk != VK_ESCAPE && vk != VK_LBUTTON && vk != VK_RBUTTON;
}

}  // namespace luma::keys
