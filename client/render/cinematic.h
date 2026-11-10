// =============================================================================
//  cinematic.h - Efectos de pantalla del zoom (barras de cine, catalejo)
// =============================================================================
//  - Barras de cine: dos franjas negras arriba y abajo, como en una película.
//    Entran y salen con una animación suave.
//  - Catalejo: todo oscuro salvo un círculo en el centro.
//  - Ocultar el HUD de Luma mientras dura el efecto.
//
//  Se dibujan con ImGui encima del juego (debajo del menú de Luma). Solo
//  cuestan algo mientras se ven.
//
//  HILOS: Set* desde cualquier hilo (atómicos); Draw en el render, con el
//  candado tomado.
// =============================================================================
#pragma once

namespace luma::render::cinematic {

// Alto de cada barra como fracción de la pantalla (0 = sin barras).
void SetBars(float heightFraction);
// Catalejo: true = mostrar el círculo.
void SetSpyglass(bool on);
// Ocultar el HUD de Luma.
void SetHideHud(bool hide);

// ¿Hay algo que dibujar (o animando)? Para el "camino rápido" del overlay.
bool Active();
// ¿El HUD de Luma debe esconderse?
bool HideHud();

void Draw();

}  // namespace luma::render::cinematic
