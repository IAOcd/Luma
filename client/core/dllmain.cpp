// =============================================================================
//  dllmain.cpp - Punto de entrada de Luma.dll
// =============================================================================
//  Fase 0: esta DLL todavía no hace nada. Solo existe para comprobar que el
//  proyecto compila. En la Fase 1 aquí arrancará Luma (logger, hilo
//  principal, tecla de descarga).
// =============================================================================

#include <windows.h>

#include "core/version.h"

// DllMain es la función que Windows llama automáticamente cuando la DLL se
// carga o se descarga de un proceso.
//
//   module  -> identificador de nuestra propia DLL dentro del proceso.
//   reason  -> por qué nos llaman (carga, descarga, nuevo hilo, ...).
//   reserved-> dato interno de Windows; no lo usamos.
//
// IMPORTANTE (para fases siguientes): dentro de DllMain Windows tiene
// "bloqueado" el cargador de DLLs. Hacer trabajo pesado aquí (cargar otras
// DLLs, esperar hilos, etc.) puede congelar el juego. Por eso en la Fase 1
// solo crearemos un hilo aparte y haremos todo el trabajo desde ese hilo.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    // Todavía no usamos estos parámetros. (void) le dice al compilador que
    // es intencional, para que no muestre una advertencia.
    (void)module;
    (void)reserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // La DLL se acaba de cargar en el juego. (Fase 1)
            break;
        case DLL_PROCESS_DETACH:
            // La DLL se está descargando. (Fase 1)
            break;
        default:
            break;
    }

    // TRUE = "todo bien, la DLL puede quedarse cargada".
    return TRUE;
}
