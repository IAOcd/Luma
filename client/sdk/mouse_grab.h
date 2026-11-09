// =============================================================================
//  mouse_grab.h - "Agarrar" y "soltar" el mouse con las funciones del juego
// =============================================================================
//  Cuando pulsas Esc (pausa) o abres el inventario o el chat, Minecraft
//  "suelta" el mouse: muestra el cursor, lo deja libre y deja de mover la
//  cámara. Al volver a jugar lo "agarra" otra vez. Lo hace con dos funciones
//  internas suyas, que encontramos así (Fase 3, con el juego 1.26.63):
//
//    1. Luma registró la pila de llamadas cada vez que el juego llamaba a
//       ShowCursor / ClipCursor (con RtlCaptureStackBackTrace; ese
//       diagnóstico se quitó después de usarlo, está explicado en el README).
//    2. Al pausar y volver a jugar aparecieron siempre dos funciones vecinas
//       del ejecutable. Desensambladas con "dumpbin /disasm":
//
//       agarrar(ventana):  ShowCursor(FALSE) hasta esconderlo, encierra el
//                          cursor en el centro de la ventana (ClipCursor),
//                          SetCapture, y pone ventana->mouseAgarrado = 1.
//       soltar(ventana):   ShowCursor(TRUE) hasta mostrarlo, ClipCursor(NULL),
//                          ReleaseCapture, y pone ventana->mouseAgarrado = 0.
//
//  Luma llama a "soltar" al abrir su menú, igual que la pausa, y a "agarrar"
//  al cerrarlo. Mientras el menú está abierto, si el juego intenta agarrar el
//  mouse otra vez, Luma lo impide (y lo hace al cerrar).
//
//  Las dos funciones se encuentran por FIRMA (sdk/scanner.h). Si una
//  actualización del juego las cambia, Init() falla, se avisa y el menú
//  sigue funcionando (pero sin bloquear la cámara).
//
//  HILOS: el juego llama a estas funciones desde el hilo de su ventana, el
//  mismo donde corre nuestra WndProc (y donde abrimos/cerramos el menú).
// =============================================================================
#pragma once

#include <windows.h>

namespace luma::sdk::mouse {

// Se llama durante Init para informar el progreso (paso actual, total), por
// ejemplo para la pantalla de carga.
using ProgressFn = void (*)(int step, int total);

// Busca las firmas y crea los hooks (activados). Llamar con MinHook listo.
bool Init(ProgressFn progress = nullptr);

// Busca el objeto de la ventana del juego a partir de su HWND (así no hay
// que esperar a que el juego agarre o suelte el mouse). Devuelve true si ya
// se conoce.
bool LocateWindow(HWND gameWindow);

// ¿Se encontraron las funciones?
bool Available();

// ¿Luma le pidió al juego que soltara el mouse al abrir el menú? (Entonces
// el juego muestra su propio cursor y ImGui no necesita dibujar otro.)
bool ReleasedByLuma();

// Llamar desde el hilo de la ventana al abrir / cerrar el menú de Luma.
void OnMenuOpened();
void OnMenuClosed();

// Antes de descargar Luma: si el menú estaba abierto, devolverle el mouse
// al juego como estaba.
void BeginUnload();

}  // namespace luma::sdk::mouse
