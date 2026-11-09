// =============================================================================
//  entity.h - Leer componentes del sistema de entidades del juego (EnTT)
// =============================================================================
//  Minecraft Bedrock guarda muchos datos de cada entidad (equipo, efectos...)
//  en una "base de datos" de la biblioteca EnTT, no dentro del objeto Actor.
//  Funciona así (lo confirmamos en 1.26.63 leyendo la memoria):
//
//    1. El registro tiene un mapa "tipo -> piscina": la clave es un hash
//       del tipo de componente (por ejemplo 0xB06141A9 = equipo). El mapa
//       es una tabla hash: cubeta = hash & (cantidad de cubetas - 1), y
//       desde ahí se sigue una cadena de nodos hasta encontrar la clave.
//    2. En la piscina, un arreglo "sparse" (por páginas de 2048) dice en qué
//       posición está el componente de cada entidad.
//    3. Los componentes están en páginas de 128; posición -> página + lugar.
//
//  Todo con lecturas seguras: si algo no cuadra, devuelve 0.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::entity {

// Dirección del componente de tipo "typeHash" (de tamaño "size") de la
// entidad "actor", o 0 si no lo tiene o no se pudo leer.
std::uintptr_t Component(std::uintptr_t actor, std::uint32_t typeHash, std::uintptr_t size);

}  // namespace luma::sdk::entity
