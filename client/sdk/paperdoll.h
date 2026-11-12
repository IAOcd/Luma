// =============================================================================
//  paperdoll.h - Mover y agrandar el "muñeco" del jugador del HUD del juego
// =============================================================================
//  El juego muestra un muñequito de tu personaje arriba a la izquierda cuando
//  corres, te agachas, nadas o vuelas (el "paper doll"). No lo dibujamos
//  nosotros: lo dibuja la interfaz del juego. Lo que hacemos es cambiarle la
//  POSICIÓN y el TAMAÑO a ese elemento de su interfaz.
//
//  CÓMO (la idea viene del client open source Latite; el código es propio):
//    - La interfaz del juego es un árbol de "controles" (UIControl), como las
//      carpetas de Windows: la pantalla "hud_screen" tiene hijos, que tienen
//      hijos... Uno se llama "hud_player": es el muñeco.
//    - Enganchamos ScreenView::setupAndRender (se llama al dibujar cada
//      pantalla de la interfaz). Después de que dibuja "hud_screen", buscamos
//      "hud_player" y le escribimos posición (+0x10) y tamaño (+0x48). El
//      frame siguiente el juego lo dibuja donde le dijimos.
//    - "Siempre visible": el renderer del muñeco tiene un temporizador que
//      dice cuánto falta para esconderlo; lo mantenemos lleno.
//
//  Offsets de UIControl (1.26.6x): +0x10 posición, +0x20 nombre (std::string),
//  +0x48 tamaño, +0x98 hijos (vector de shared_ptr), +0xB8 componentes.
//  Todo con lecturas/escrituras seguras: si algo no cuadra, no se toca nada.
// =============================================================================
#pragma once

namespace luma::sdk::paperdoll {

bool Init();
bool Available();

struct Settings {
    bool enabled = false;
    float x = 0.0f;       // esquina superior izquierda, en UNIDADES de la interfaz del juego
    float y = 0.0f;
    float scale = 1.0f;   // 1 = tamaño normal del juego
    bool alwaysShow = true;
};
void Set(const Settings& s);

// Ancho de la pantalla del HUD en unidades de la interfaz del juego (0 si
// todavía no se conoce). Píxeles por unidad = ancho en píxeles / esto.
float RootWidth();
// ¿Ya encontramos el muñeco en la interfaz del juego?
bool Found();

// THE GAME'S SCREENS: every screen the game draws (the HUD, the chat, the
// inventory, the pause menu...) has a name at the root of its tree. Each new
// name is written to the log once. If one has "death" in it, it's the "You
// died!" screen: this says how many times it was drawn (it only grows), so
// a module can notice the moment you die.
unsigned DeathScreenFrames();
// How many times a "menu" screen was drawn (pause, chat, inventory... not
// the ones always on top of the HUD: crosshair, debug, toasts). It only grows.
unsigned MenuScreenFrames();
// How many times the game drew its HUD ("hud_screen"). It stops growing when
// the HUD is hidden (F1): render/hud follows it. It only grows.
unsigned HudScreenFrames();
// The same for the crosshair's own screen ("hud_crosshair_screen").
unsigned CrosshairScreenFrames();

// Devuelve el muñeco a su lugar original (al desactivar o al descargar Luma).
void BeginUnload();

}  // namespace luma::sdk::paperdoll
