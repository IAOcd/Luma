// =============================================================================
//  icons.h - Íconos SVG de Luma (vectoriales)
// =============================================================================
//  Cada ícono es un "path" de SVG: el mismo texto que va en <path d="..."/>
//  dentro de un archivo .svg, dibujado en una cuadrícula de 24x24. Ejemplo,
//  una flecha hacia atrás:
//
//      "M15 5L8 12L15 19"   (mover a 15,5; línea a 8,12; línea a 15,19)
//
//  Luma lee ese texto (una vez, al usarlo por primera vez) y lo dibuja con
//  líneas y curvas de ImGui. Por ser vectores, se ven nítidos a cualquier
//  tamaño y no ocupan texturas.
//
//  Comandos soportados (mayúscula = absoluto, minúscula = relativo):
//    M m (mover), L l (línea), H h / V v (línea horizontal / vertical),
//    C c (curva cúbica), S s (curva cúbica suave), Q q (curva cuadrática),
//    Z z (cerrar). Para círculos se usan cuatro curvas C.
//  Si el texto empieza con "F ", la forma se RELLENA en vez de trazarse.
//
//  Todos los íconos son propios (diseñados para Luma).
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::icons {

// --- Íconos de Luma (paths SVG, cuadrícula 24x24) --------------------------------
inline constexpr const char* kAll = "M4 4h6v6H4z M14 4h6v6h-6z M4 14h6v6H4z M14 14h6v6h-6z";
inline constexpr const char* kHud = "M3 5h18v14H3z M3 9h18 M7 13h6 M7 16h4";
inline constexpr const char* kVisual =
    "M2 12C4.5 7 8 5 12 5C16 5 19.5 7 22 12C19.5 17 16 19 12 19C8 19 4.5 17 2 12Z "
    "M15 12C15 13.66 13.66 15 12 15C10.34 15 9 13.66 9 12C9 10.34 10.34 9 12 9C13.66 9 15 10.34 15 12Z";
inline constexpr const char* kUtility =
    "M4 7h9 M17 7h3 M15 5v4 M4 12h3 M11 12h9 M9 10v4 M4 17h11 M19 17h1 M17 15v4";
inline constexpr const char* kWorld =
    "M21 12C21 16.97 16.97 21 12 21C7.03 21 3 16.97 3 12C3 7.03 7.03 3 12 3C16.97 3 21 7.03 21 12Z "
    "M3 12h18 M12 3C14.5 5.5 15.5 8.7 15.5 12C15.5 15.3 14.5 18.5 12 21 "
    "M12 3C9.5 5.5 8.5 8.7 8.5 12C8.5 15.3 9.5 18.5 12 21";
inline constexpr const char* kPerformance =
    "M4 17C4 12.58 7.58 9 12 9C16.42 9 20 12.58 20 17 M12 17L15.5 12.5 M4 17h2 M18 17h2";
inline constexpr const char* kTheme =
    "M12 3C12 3 6 9.5 6 14C6 17.31 8.69 20 12 20C15.31 20 18 17.31 18 14C18 9.5 12 3 12 3Z M9 14.5C9 16 10 17 11.5 17";
inline constexpr const char* kProfiles =
    "M15.5 8C15.5 9.93 13.93 11.5 12 11.5C10.07 11.5 8.5 9.93 8.5 8C8.5 6.07 10.07 4.5 12 4.5"
    "C13.93 4.5 15.5 6.07 15.5 8Z M5 20C5 16.5 8.13 14 12 14C15.87 14 19 16.5 19 20";
inline constexpr const char* kSettings =
    "M15 12C15 13.66 13.66 15 12 15C10.34 15 9 13.66 9 12C9 10.34 10.34 9 12 9C13.66 9 15 10.34 15 12Z "
    "M12 3v3 M12 18v3 M3 12h3 M18 12h3 M5.6 5.6l2.1 2.1 M16.3 16.3l2.1 2.1 M5.6 18.4l2.1-2.1 M16.3 7.7l2.1-2.1";
inline constexpr const char* kDiagnostics = "M3 12h4l2.5-6 5 12 2.5-6h4";
inline constexpr const char* kSearch =
    "M17 10.5C17 14.09 14.09 17 10.5 17C6.91 17 4 14.09 4 10.5C4 6.91 6.91 4 10.5 4C14.09 4 17 6.91 17 10.5Z M15.5 15.5L20 20";
inline constexpr const char* kBack = "M15 5L8 12L15 19";
inline constexpr const char* kMove =
    "M12 3v18 M3 12h18 M9 6l3-3 3 3 M9 18l3 3 3-3 M6 9l-3 3 3 3 M18 9l3 3-3 3";
inline constexpr const char* kSparkle = "M12 3L13.8 10.2L21 12L13.8 13.8L12 21L10.2 13.8L3 12L10.2 10.2Z";
inline constexpr const char* kChart = "M4 4v16h16 M8 15l3.5-4 3 2.5L19 8";
// El logo de Luma (la "L" orgánica), como forma RELLENA: la "F" del principio
// le dice a Draw que lo pinte lleno en vez de trazar su borde. Calcado a mano
// del PNG del logo y pasado a la cuadrícula de 24x24.
inline constexpr const char* kLumaLogo =
    "F M2.3 8.5 C2.3 5.8 4.2 4.8 6.2 4.8 C8.4 4.8 9.8 6.3 9.6 8.3 C9.4 10.3 8.3 11.6 8.4 13.3 "
    "C8.5 14.4 9.6 14.3 10.4 13.6 C11.6 12.4 12.8 11.6 14.4 11.6 C16.2 11.6 17.4 12.8 18.2 14.0 "
    "C18.6 14.5 19.0 14.2 19.6 14.0 C21.0 13.7 22.2 14.9 22.1 16.4 C22.0 18.3 20.4 19.4 18.6 18.4 "
    "C16.6 17.3 14.6 16.3 12.4 16.6 C10.0 16.9 8.6 18.6 5.8 18.6 C3.6 18.6 2.3 17.1 2.3 15.0 Z";
// Vista de lista (módulos apilados) y casa (volver al inicio radial).
inline constexpr const char* kList = "M4 6h16 M4 12h16 M4 18h16";
inline constexpr const char* kHome = "M4 11L12 4L20 11 M6 9.5V20h12V9.5 M10 20v-5h4v5";
inline constexpr const char* kCheck = "M5 12.5l4.5 4.5L19 7.5";
inline constexpr const char* kClose = "M6 6l12 12 M18 6L6 18";
inline constexpr const char* kKey =
    "M3 7h18v10H3z M7 11h1 M11 11h2 M16 11h1 M8 14h8";

// Dibuja un path SVG centrado en "center", de lado "size" (la cuadrícula de
// 24x24 se escala a ese tamaño). "thickness" = grosor de línea en píxeles
// (0 = automático, proporcional al tamaño).
void Draw(ImDrawList* draw, const char* svgPath, ImVec2 center, float size, ImU32 color,
          float thickness = 0.0f);

}  // namespace luma::render::icons
