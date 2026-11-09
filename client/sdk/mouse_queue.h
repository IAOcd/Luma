// =============================================================================
//  mouse_queue.h - Vaciar la cola de acciones del mouse del juego
// =============================================================================
//  CÓMO RECIBE EL MOUSE MINECRAFT (lo que nos costó tanto encontrar):
//
//  El juego tiene un objeto global "MouseDevice". Dentro guarda una LISTA
//  (std::vector) de "acciones de mouse" pendientes: cada movimiento, cada
//  clic, cada giro de rueda. Da igual de dónde venga el mouse (GameInput,
//  mensajes de Windows...): todo termina en esa lista. En cada tick, una
//  función del juego la recorre y hace lo que corresponda (girar la cámara,
//  atacar, usar un ítem...).
//
//  Por eso ningún bloqueo de funciones de Windows alcanzaba: la cámara y los
//  clics llegaban a esa lista por un camino interno de GameInput.
//
//  Solución (la idea viene de estudiar los clients open source Latite y
//  Flarial; el código y las firmas de Luma son propios): hookear esa función
//  y, con el menú de Luma abierto, quitar de la lista los clics y la rueda
//  ANTES de que el juego la procese. Los movimientos se dejan pasar (con el
//  mouse "soltado" no giran la cámara, y así la cámara no salta al cerrar).
//
//  Cada acción ocupa 20 bytes (en el juego 1.26.63):
//     +0  x, +2 y, +4 dx, +6 dy   (números de 16 bits)
//     +8  tipo de acción  (0 = movimiento, 1 = izquierdo, 2 = derecho,
//                          3 = central, 4 = rueda)
//     +9  dato            (1 = apretado, 0 = soltado; o el giro de la rueda)
//     +12 id del puntero, +16 indicador "puntero sin movimiento"
//  Lo confirma el propio código: divide (final − inicio) por 20.
// =============================================================================
#pragma once

namespace luma::sdk::mousequeue {

// Busca la función por firma y la hookea. Si no se encuentra, el menú
// funciona igual pero el juego seguiría recibiendo clics con el menú abierto.
bool Init();

// ¿Está activo el bloqueo?
bool Available();

// Antes de descargar Luma.
void BeginUnload();

}  // namespace luma::sdk::mousequeue
