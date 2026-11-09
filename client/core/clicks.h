// =============================================================================
//  clicks.h - Registro de clics del mouse (para CPS y keystrokes)
// =============================================================================
//  CPS = "clics por segundo". Guardamos el momento de cada clic en un
//  "ring buffer" (arreglo fijo que se va pisando) y contamos cuántos hay en
//  el último segundo. Sin pedir memoria y sin candados (variables atómicas).
//
//  ¿De dónde salen los clics? Del mismo lugar donde los lee el juego: su cola
//  de acciones del mouse (sdk/mouse_queue). Si esa firma no se encontrara,
//  de los mensajes de Windows (WM_LBUTTONDOWN...) como respaldo.
//
//  RENDIMIENTO: solo se registran clics si algún módulo activo los usa
//  (Acquire/Release). Si no, el hook de la cola ni mira los clics.
// =============================================================================
#pragma once

namespace luma::clicks {

enum class Button { Left = 0, Right = 1 };

// Un módulo que necesita los clics llama a Acquire al activarse y a Release
// al desactivarse.
void Acquire();
void Release();
bool Wanted();

// Un clic (botón apretado). Desde cualquier hilo.
void Record(Button button);

// Clics en el último segundo.
int PerSecond(Button button);

}  // namespace luma::clicks
