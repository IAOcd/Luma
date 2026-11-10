// =============================================================================
//  sky.h - El cielo pixelado del cartel de inicio (generado por código)
// =============================================================================
//  Inspirado en el estilo de LabyMod: un cielo de "píxeles grandes" con
//  degradado, estrellas que titilan, nubes de bloques que avanzan despacio,
//  un sol (o una luna) de bloques y una franja de suelo abajo. Nada es una
//  imagen: todo son rectángulos que se calculan en cada frame (es muy barato).
//
//  Sigue la HORA de tu PC (amanecer, día, atardecer, noche) con transiciones
//  suaves, o se puede fijar en Ajustes.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::launcher::gui::sky {

enum class Mode { Auto = 0, Day = 1, Sunset = 2, Night = 3 };

// Dibuja el cielo en [min, max]. "pixel" = tamaño de cada "píxel" del dibujo
// (en píxeles de pantalla). "rounding" = esquinas redondeadas del cartel.
// "background" = color de lo que hay detrás (para recortar las esquinas).
void Draw(ImDrawList* d, ImVec2 min, ImVec2 max, float pixel, float rounding, ImU32 background, Mode mode);

}  // namespace luma::launcher::gui::sky
