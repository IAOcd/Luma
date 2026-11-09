// =============================================================================
//  hooks.h - Envoltorio sobre MinHook
// =============================================================================
//  ¿Qué es un hook? Es "interceptar" una función: cada vez que el juego la
//  llama, primero se ejecuta NUESTRA función (el "detour"), que puede hacer
//  algo y después llamar a la función original (a través del "trampolín"
//  que MinHook nos da).
//
//      juego --llama--> Present  ==hook==>  nuestro detour
//                                             | dibuja Luma
//                                             v
//                                           trampolín --> Present original
//
//  MinHook lo logra reescribiendo los primeros bytes de la función original
//  con un salto hacia nuestro detour. Al desactivar el hook, restaura esos
//  bytes y todo vuelve a quedar como estaba.
//
//  Este archivo además lleva la cuenta de cuántos hilos están DENTRO de algún
//  detour en este momento. Al descargar Luma hay que esperar a que esa cuenta
//  llegue a 0: si quitáramos la DLL de la memoria mientras un hilo del juego
//  todavía está ejecutando nuestro código, el juego crashearía.
// =============================================================================
#pragma once

#include <atomic>

namespace luma::hooks {

// Prepara MinHook. Llamar una vez al arrancar.
bool Init();

// Crea un hook (todavía desactivado).
//   target   -> la función a interceptar.
//   detour   -> nuestra función de reemplazo.
//   original -> aquí MinHook escribe la dirección del trampolín, que usamos
//               para llamar a la función original.
//   name     -> nombre para el log.
bool Create(void* target, void* detour, void** original, const char* name);

// Activa / desactiva todos los hooks creados.
bool EnableAll();

// Activa un solo hook (para hooks que se crean más tarde, con Luma ya
// funcionando).
bool Enable(void* target, const char* name);
void DisableAll();

// Espera a que ningún hilo esté dentro de un detour. Ver ActiveCallGuard.
void WaitForActiveCalls();

// Libera MinHook (quita los hooks del todo). Llamar después de DisableAll y
// WaitForActiveCalls.
void Shutdown();

// --- Contador de llamadas activas ---------------------------------------------
// Cada detour crea uno de estos al empezar. El constructor suma 1 al contador
// y el destructor (que C++ llama automáticamente al salir de la función, por
// cualquier camino) resta 1.
//
// "std::atomic" es un número que varios hilos pueden modificar a la vez sin
// corromperlo.
extern std::atomic<int> g_activeCalls;

struct ActiveCallGuard {
    ActiveCallGuard() { g_activeCalls.fetch_add(1, std::memory_order_acq_rel); }
    ~ActiveCallGuard() { g_activeCalls.fetch_sub(1, std::memory_order_acq_rel); }
    ActiveCallGuard(const ActiveCallGuard&) = delete;
    ActiveCallGuard& operator=(const ActiveCallGuard&) = delete;
};

}  // namespace luma::hooks
