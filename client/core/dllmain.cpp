// =============================================================================
//  dllmain.cpp - Punto de entrada de Luma.dll
// =============================================================================
//  Windows llama a DllMain cuando la DLL entra o sale del proceso del juego.
//  Aquí hacemos lo MÍNIMO posible: crear el hilo propio de Luma y nada más.
//
//  ¿Por qué? Mientras DllMain se ejecuta, Windows tiene tomado un candado
//  interno llamado "loader lock". Si dentro de DllMain hiciéramos cosas como
//  cargar otras DLLs o esperar a otros hilos, podríamos quedar esperando algo
//  que a su vez espera ese candado: el juego se congelaría para siempre
//  (un "deadlock"). Por eso todo el trabajo real va en MainThread.
// =============================================================================

#include <windows.h>

#include "core/luma.h"

namespace {

// Función que ejecuta el hilo de Luma. Windows la arranca recién cuando
// DllMain terminó, así que acá ya podemos hacer cualquier cosa.
DWORD WINAPI MainThread(LPVOID parameter) {
    HMODULE module = static_cast<HMODULE>(parameter);

    // "try / catch (...)" atrapa cualquier error de C++ (excepción) que se
    // escape. Una excepción sin atrapar dentro del juego lo cerraría de golpe.
    try {
        luma::Run(module);
    } catch (...) {
        // No hay mucho más que hacer: seguimos al paso de descarga.
    }

    // Quita Luma.dll de la memoria del juego y termina este hilo, en un solo
    // paso. Tiene que ser una única llamada: si primero liberáramos la DLL y
    // después intentáramos terminar el hilo, esa segunda instrucción ya no
    // existiría en memoria (formaba parte de la DLL) y el juego crashearía.
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // Le decimos a Windows que no nos avise cada vez que el juego crea
            // o termina un hilo: no nos interesa y así ahorramos trabajo.
            DisableThreadLibraryCalls(module);

            HANDLE thread = CreateThread(nullptr, 0, MainThread, module, 0, nullptr);
            if (thread == nullptr) {
                // Sin hilo no hay Luma. Devolver FALSE hace que Windows
                // descarte la DLL y el launcher reporta que falló.
                return FALSE;
            }
            // No necesitamos esperar ni controlar el hilo desde aquí, así que
            // soltamos nuestra referencia (el hilo sigue corriendo igual).
            CloseHandle(thread);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Llegamos aquí en dos casos:
            //  a) Descarga normal (reserved == nullptr): Shutdown() ya limpió
            //     todo antes de FreeLibraryAndExitThread. Nada que hacer.
            //  b) El juego se está cerrando (reserved != nullptr): Windows ya
            //     terminó a la fuerza todos los hilos, incluido el nuestro,
            //     quizá en mitad de algo. Tocar el log o cualquier candado
            //     podría congelar el cierre del juego, así que NO hacemos nada.
            //     El log ya está a salvo porque cada línea se guarda al instante.
            (void)reserved;
            break;
        default:
            break;
    }
    return TRUE;
}
