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
//  Además hookeamos PeekMessageW, GetAsyncKeyState/GetKeyState y ClipCursor
//  (ver input.cpp). El mouse del juego (cámara y clics) no pasa por aquí: lo
//  resuelven sdk/mouse_grab y sdk/mouse_queue.
// =============================================================================
#pragma once

#include <windows.h>

namespace luma::render::input {

// Crea los hooks de PeekMessageW, GetAsyncKeyState, GetKeyState y ClipCursor
// (se activan con hooks::EnableAll).
bool CreateHooks();

// Instala nuestra WndProc en la ventana del juego.
bool Install(HWND gameWindow);

// Llamar cuando el menú se abre o se cierra: libera o restaura el cursor.
void OnMenuToggled(bool open);

// Devuelve la WndProc original y restaura el cursor. Llamar antes de
// descargar Luma, con los hooks ya desactivados.
void Uninstall();

}  // namespace luma::render::input
