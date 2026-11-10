// =============================================================================
//  hud_ui.h - Tocar elementos de la interfaz del juego: coordenadas y scoreboard
// =============================================================================
//  La interfaz del juego es un árbol de "controles" (ver sdk/paperdoll.h). Leí
//  ese árbol en la memoria del juego (1.26.63) y estos son los que nos sirven:
//
//      hud_screen
//        ...
//          sidebar              <- el scoreboard del juego (a la derecha)
//            main / displayed_objective / lists (players, scores)
//          chat_stack
//            player_position    <- las coordenadas del juego ("Posición: ...")
//              player_position_text
//
//  Algunos servidores traen su propio scoreboard con su paquete de recursos
//  (por ejemplo "hcf_sidebar"); también lo buscamos.
//
//  CÓMO se mueven: cada control guarda su posición en pantalla (+0x10) y la
//  relativa a su padre (+0x40). Después de que el juego dibuja el HUD,
//  sumamos el desplazamiento a TODO el subárbol (el scoreboard tiene hijos:
//  título, nombres, puntos). Si el juego vuelve a acomodarlo (cambian los
//  puntos), lo detectamos y lo volvemos a mover.
//  Esconder = moverlo muy lejos, fuera de la pantalla.
//
//  Todo con lecturas/escrituras seguras: si algo no cuadra, no se toca nada.
//  HILOS: OnHudScreen corre en el hilo de la interfaz del juego (lo llama el
//  hook de sdk/paperdoll); el resto se puede llamar desde cualquier hilo.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::hudui {

// Engancha UIControl::updatePos (para mover el scoreboard sin que titile).
bool Init();

// Lo llama el hook de ScreenView::setupAndRender con la raíz "hud_screen".
void OnHudScreen(std::uintptr_t root);
// Lo mismo, ANTES de que el juego dibuje el HUD (solo vuelve a poner el scoreboard).
void BeforeHudRender(std::uintptr_t root);

// Esconder las coordenadas del juego (las de la regla "Mostrar coordenadas").
void SetHideCoords(bool hide);

// Mover el scoreboard: esquina superior izquierda en UNIDADES de la interfaz
// del juego (como sdk::paperdoll).
void SetScoreboard(bool enabled, float x, float y);
// Tamaño del scoreboard visto (unidades de la interfaz); 0 si no hay ninguno.
float ScoreboardWidth();
float ScoreboardHeight();

// Devuelve todo a su lugar (al descargar Luma).
void BeginUnload();

}  // namespace luma::sdk::hudui
