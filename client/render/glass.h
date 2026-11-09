// =============================================================================
//  glass.h - Dibujar superficies de "vidrio líquido"
// =============================================================================
//  Un panel de vidrio se arma con capas, de abajo hacia arriba:
//
//    1. Sombra suave debajo (varias capas con poca opacidad).
//    2. El JUEGO DESENFOCADO, recortado exactamente a la forma del panel
//       (la textura que prepara render/gpu cada frame).
//    3. Un tinte translúcido (ahumado en oscuro, lechoso en claro).
//    4. Un brillo arriba que se desvanece hacia abajo (luz reflejada).
//    5. Un borde fino que capta la luz: más brillante arriba, tenue abajo.
//
//  Todo es "dibujar rectángulos redondeados con ImGui", así que cuesta muy
//  poco; lo caro (el desenfoque) lo hace la GPU una vez por frame.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::glass {

// Panel grande de vidrio. "alpha" multiplica todo (para las animaciones).
void Panel(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float alpha);

// Superficie interior (tarjetas, cajas): translúcida, con brillo y borde, sin
// volver a dibujar el desenfoque. "hover" (0..1) la aclara.
void Surface(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float alpha, float hover);

// Cambia la transparencia de los vértices dibujados desde "firstVertex" según
// su altura: "alphaTop" arriba (en topY) y "alphaBottom" abajo (en bottomY).
// Así hacemos degradados en formas redondeadas (ImGui no los trae).
void VerticalFade(ImDrawList* draw, int firstVertex, float topY, float bottomY, float alphaTop,
                  float alphaBottom);

}  // namespace luma::render::glass
