// =============================================================================
//  keys.cpp - Nombres de teclas
// =============================================================================
#include "core/keys.h"
#include "core/tr.h"

#include <windows.h>

#include <format>

namespace luma::keys {

std::string Name(int vk) {
    if (vk == kNone) {
        return Tr("None");
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
        case VK_DELETE:   return Tr("Supr");
        case VK_HOME:     return Tr("Home");
        case VK_END:      return Tr("End");
        case VK_PRIOR:    return Tr("Page Up");
        case VK_NEXT:     return Tr("Page Down");
        case VK_UP:       return Tr("Up arrow");
        case VK_DOWN:     return Tr("Down arrow");
        case VK_LEFT:     return Tr("Left arrow");
        case VK_RIGHT:    return Tr("Right arrow");
        case VK_SPACE:    return Tr("Space");
        case VK_TAB:      return "Tab";
        case VK_RETURN:   return "Enter";
        case VK_BACK:     return Tr("Backspace");
        case VK_SHIFT:    return "Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_MENU:     return "Alt";
        case VK_CAPITAL:  return Tr("Caps Lock");
        case VK_PAUSE:    return Tr("Pause");
        case VK_SCROLL:   return Tr("Scroll Lock");
        case VK_MULTIPLY: return "Num *";
        case VK_ADD:      return "Num +";
        case VK_SUBTRACT: return "Num -";
        case VK_DECIMAL:  return "Num .";
        case VK_DIVIDE:   return "Num /";
        case VK_OEM_3:    return Tr("` key");
        case VK_OEM_MINUS: return Tr("- key");
        case VK_OEM_PLUS: return Tr("+ key");
        case VK_OEM_COMMA: return Tr(", key");
        case VK_OEM_PERIOD: return Tr(". key");
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_MBUTTON:  return Tr("Middle click");
        default:          return TrFormat("Key 0x{:02X}", vk);
    }
}

bool IsBindable(int vk) {
    return vk > 0 && vk < 256 && vk != VK_ESCAPE && vk != VK_LBUTTON && vk != VK_RBUTTON;
}

}  // namespace luma::keys
