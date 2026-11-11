// =============================================================================
//  watch.h - Diagnóstico: "¿quién escribe en esta dirección?"
// =============================================================================
//  Usa los registros de depuración del procesador (DR0..DR3): se le pide a la
//  CPU que avise (con una excepción "single step") justo después de que una
//  instrucción ESCRIBE en una dirección. Un manejador de excepciones de Luma
//  anota desde qué instrucción del juego vino la escritura y deja seguir.
//  Es lo mismo que el "Find out what writes to this address" de Cheat Engine.
//
//  Solo para investigar (encontrar funciones del juego sin sus nombres): se
//  activa unos segundos y se apaga solo. No cambia nada del juego.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::watch {

// Vigila escrituras de 8 bytes en "address0" (y "address1" si no es 0) en el
// hilo "threadId" (0 = en TODOS los hilos del juego) durante "ms" milisegundos; después escribe en el log qué
// instrucciones escribieron (relativas al juego). Solo se puede una vez.
// Hasta 4 direcciones (las que valen 0 no se vigilan).
void Start(std::uint32_t threadId, std::uintptr_t address0, std::uintptr_t address1, int ms, const char* label,
           std::uintptr_t address2 = 0, std::uintptr_t address3 = 0);

}  // namespace luma::sdk::watch
