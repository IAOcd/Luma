// =============================================================================
//  luma.h - Ciclo de vida de Luma (arrancar, esperar, descargar)
// =============================================================================
#pragma once

#include <windows.h>

namespace luma {

// Corre todo Luma: arranca los sistemas, espera hasta que el usuario pulse la
// tecla de descarga y luego apaga todo en orden. Se ejecuta en el hilo propio
// de Luma (ver dllmain.cpp), nunca dentro de DllMain.
//
// Cuando esta función termina, ya no queda NADA de Luma activo en el juego
// (ni hooks, ni hilos, ni ventanas), y es seguro quitar la DLL de la memoria.
void Run(HMODULE module);

}  // namespace luma
