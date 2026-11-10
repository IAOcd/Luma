// =============================================================================
//  theme.h - Colores del launcher (temas oscuro y claro, sacados del logo)
// =============================================================================
//  El logo de Luma tiene dos colores: carbón (#25282B) y marfil (#E9E6DF).
//    - Tema OSCURO: fondos carbón, acentos marfil.
//    - Tema CLARO: lo inverso.
//  Verde, ámbar y rojo apagados SOLO para estados (cargado, esperando, error),
//  para que no compitan con la marca.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::launcher::gui {

enum class ThemeKind { Dark = 0, Light = 1 };

struct Palette {
    ImVec4 background;   // fondo de la ventana
    ImVec4 sidebar;      // barra lateral
    ImVec4 card;         // tarjetas
    ImVec4 cardHover;
    ImVec4 border;       // bordes finos de tarjetas
    ImVec4 text;
    ImVec4 textDim;      // texto secundario
    ImVec4 accent;       // botón JUGAR, sección activa
    ImVec4 onAccent;     // texto sobre el acento
    ImVec4 heroTop;      // degradado del cartel de inicio
    ImVec4 heroBottom;
    ImVec4 heroShape;    // las formas del logo dentro del cartel
    ImVec4 success;
    ImVec4 warning;
    ImVec4 danger;
};

void SetTheme(ThemeKind kind);
ThemeKind CurrentTheme();
const Palette& P();

// Color con transparencia extra (alpha multiplicado).
inline ImU32 Col(const ImVec4& c, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}
// Mezcla dos colores (t = 0 -> a, t = 1 -> b).
inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

}  // namespace luma::launcher::gui
