// =============================================================================
//  memory.h - Leer memoria del juego SIN poder crashear
// =============================================================================
//  Cuando leemos un dato del juego (la posición del jugador, por ejemplo)
//  seguimos punteros: "el jugador está en X; en X+0x240 hay un puntero a su
//  posición...". Si el juego liberó ese objeto (saliste del mundo) o una
//  actualización movió los campos, el puntero podría apuntar a memoria que
//  ya no existe, y leerla CRASHEARÍA el juego.
//
//  Estas funciones leen dentro de un bloque __try/__except de Windows (SEH):
//  si la lectura falla, devuelven false en vez de crashear.
//
//  REGLA del proyecto: toda lectura de memoria del juego pasa por aquí.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace luma::sdk {

// Copia "size" bytes desde "address" a "out". false si la memoria no se pudo leer.
bool SafeCopy(std::uintptr_t address, void* out, std::size_t size);

// Escribe "size" bytes (false si esa memoria no se pudo escribir).
bool SafeWrite(std::uintptr_t address, const void* data, std::size_t size);

template <typename T>
bool SafeRead(std::uintptr_t address, T& out) {
    return SafeCopy(address, &out, sizeof(T));
}

// Lee un std::string del juego (formato de Microsoft: si mide menos de 16,
// el texto está dentro del propio objeto; si no, hay un puntero a él).
// Copia como mucho outSize-1 caracteres y termina en '\0'.
bool SafeReadString(std::uintptr_t address, char* out, std::size_t outSize);

// ¿Parece un puntero válido a memoria normal? (descarta 0, valores chicos y
// direcciones fuera del espacio de usuario de 64 bits).
inline bool LooksLikePointer(std::uintptr_t value) {
    return value > 0x10000 && value < 0x00007FFFFFFFFFFF && (value & 0x7) == 0;
}

}  // namespace luma::sdk
