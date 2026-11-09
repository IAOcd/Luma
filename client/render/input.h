// =============================================================================
//  input.h - Captura de teclado y mouse
// =============================================================================
//  Windows le entrega el teclado y el mouse a cada ventana como "mensajes"
//  (WM_KEYDOWN, WM_MOUSEMOVE, ...), que procesa una función de la ventana
//  llamada WndProc. Reemplazamos la WndProc del juego por la nuestra
//  ("subclassing"):
//
//    Windows --mensaje--> nuestra WndProc --(si el menú está cerrado)--> WndProc del juego
//                              |
//                              +--(si el menú está abierto)--> ImGui (el juego no lo ve)
//
//  Así detectamos la tecla del menú sin gastar nada por frame (solo
//  reaccionamos cuando llega el mensaje).
//
//  PERO Minecraft no lee el movimiento del mouse por mensajes, sino con
//  "raw input" (GetRawInputData), y además encierra el cursor dentro de la
//  ventana con ClipCursor. Por eso también hookeamos esas dos funciones de
//  Windows (ver input.cpp).
// =============================================================================
#pragma once

#include <windows.h>

namespace luma::render::input {

// Crea los hooks de GetRawInputData y ClipCursor (se activan con
// hooks::EnableAll).
bool CreateHooks();

// Instala nuestra WndProc en la ventana del juego.
bool Install(HWND gameWindow);

// Llamar cuando el menú se abre o se cierra: libera o restaura el cursor.
void OnMenuToggled(bool open);

// Devuelve la WndProc original y restaura el cursor. Llamar antes de
// descargar Luma, con los hooks ya desactivados.
void Uninstall();

}  // namespace luma::render::input
