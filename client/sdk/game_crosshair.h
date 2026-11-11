// =============================================================================
//  game_crosshair.h - Esconder la mira del juego
// =============================================================================
//  La mira del juego no es un control del árbol de la interfaz: la dibuja un
//  "renderer" propio de la interfaz (una clase del juego con su tabla de
//  funciones). Lo encontramos buscando quién carga la textura
//  "textures/ui/cross_hair" (juego+0x5733E10 en 1.26.63): es su función de
//  DIBUJAR (la 16 de su tabla, la "render" de los renderers de Minecraft).
//
//  La enganchamos: si el módulo de mira pide esconderla, no la dejamos
//  dibujar. No se toca la interfaz ni los archivos del juego.
//
//  Firma: la instrucción que arma el texto "oss_hair" (parte de "cross_hair")
//  es única; la función empieza 0x35F bytes antes (y lo comprobamos).
// =============================================================================
#pragma once

namespace luma::sdk::gamecrosshair {

bool Init();
bool Available();
void SetHidden(bool hidden);

}  // namespace luma::sdk::gamecrosshair
