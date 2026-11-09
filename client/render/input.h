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

// La ventana del juego (nullptr hasta que se instala).
HWND GameWindow();

// Abrir/cerrar el menú desde cualquier hilo: se lo pide a la ventana (el
// menú siempre se abre y se cierra desde el hilo de la ventana).
void RequestMenuToggle();

// --- Teclas "sostenidas" por Luma (toggle sprint / toggle sneak) -----------------
//  El juego lleva la cuenta de qué teclas están apretadas con los mensajes de
//  la ventana (WM_KEYDOWN / WM_KEYUP). Para "sostener" Ctrl sin que lo
//  apretes, Luma le manda al juego un WM_KEYDOWN de Ctrl y se guarda el
//  WM_KEYUP real cuando lo sueltas. Para el juego, la tecla sigue apretada.
//
//  - Pulsar la tecla de verdad lo alterna (como en otros clients).
//  - En los menús del juego (inventario, chat, pausa) se suelta sola: si no,
//    un Shift sostenido haría que cada clic en el inventario mueva ítems.
//  - Si la ventana pierde el foco, se suelta; al volver, se vuelve a apretar.
enum class HoldSlot { Sprint = 0, Sneak = 1 };
// enabled = el módulo está activo; vk = la tecla del juego (VK_CONTROL...).
void ConfigureHold(HoldSlot slot, bool enabled, int vk);
// ¿La está sosteniendo ahora? (para el texto del HUD)
bool IsHoldActive(HoldSlot slot);
// Hilo principal de Luma, cada 50 ms: suelta/aprieta según si estás en un
// menú del juego o la ventana tiene el foco.
void PollHolds();

}  // namespace luma::render::input
