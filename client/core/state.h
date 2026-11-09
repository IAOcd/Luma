// =============================================================================
//  state.h - El candado del estado de Luma
// =============================================================================
//  Varios hilos del juego tocan datos de Luma:
//    - el hilo de render (dibuja el menú y el HUD),
//    - el hilo de la ventana (teclas, mouse),
//    - el hilo principal de Luma (guarda la configuración).
//
//  Para que nunca lean algo a medio escribir, TODO acceso a módulos,
//  configuración e ImGui se hace con este candado tomado:
//
//      std::lock_guard lock(luma::StateMutex());
//      ...leer o cambiar módulos/config...
//
//  Es "recursive" para que el mismo hilo pueda tomarlo más de una vez (por
//  ejemplo, una función que lo toma llama a otra que también lo toma).
//
//  Regla para evitar bloqueos: nunca tenerlo tomado mientras se llama a una
//  función ORIGINAL del juego o de DirectX.
// =============================================================================
#pragma once

#include <mutex>

namespace luma {

inline std::recursive_mutex& StateMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

}  // namespace luma
