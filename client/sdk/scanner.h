// =============================================================================
//  scanner.h - Escaneo de firmas en la memoria del juego
// =============================================================================
//  Las funciones internas de Minecraft no tienen nombre ni dirección fija:
//  cada actualización del juego las mueve. Lo que casi no cambia es su
//  CÓDIGO (los bytes de las instrucciones). Una "firma" es un trozo de esos
//  bytes que identifica a la función:
//
//      "56 57 48 83 EC 28 48 89 CE B9 01 00 00 00 FF 15 ?? ?? ?? ??"
//
//  Los "??" son comodines: bytes que cambian entre versiones (direcciones,
//  desplazamientos) y que no hay que comparar. El escáner recorre el código
//  del juego buscando el lugar donde coinciden todos los demás.
//
//  Si una firma no se encuentra (por ejemplo, tras una actualización que
//  cambió esa función), el escáner devuelve 0 y el módulo que la necesita
//  se desactiva y avisa. Nunca se adivina una dirección.
//
//  RENDIMIENTO: escanear ~250 MB de código tarda del orden de décimas de
//  segundo por firma. Se hace UNA vez al cargar Luma, en el hilo principal.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace luma::sdk {

struct ScanResult {
    std::uintptr_t address = 0;  // 0 = no encontrada
    int matches = 0;             // cuántas veces aparece (lo correcto es 1)
};

// Busca la firma en la sección de código (.text) de Minecraft.Windows.exe.
// Si aparece más de una vez, devuelve la primera y matches > 1 (señal de que
// la firma es demasiado corta y podría apuntar a la función equivocada).
ScanResult FindPattern(std::string_view pattern);

// Todas las veces que aparece la firma (como mucho "max"), en orden. Para
// familias de funciones casi iguales (ver sdk/game_options.cpp).
std::vector<std::uintptr_t> FindAllPatterns(std::string_view pattern, std::size_t max);

// Dirección del módulo del juego en memoria (para mostrar desplazamientos).
std::uintptr_t GameBase();

// Muchas instrucciones x64 apuntan a una variable "relativa a sí mismas":
//     mov r15, [rip + desplazamiento]
// donde rip = la dirección de la instrucción SIGUIENTE. Dada la dirección de
// los 4 bytes del desplazamiento y dónde termina la instrucción, devuelve la
// dirección real de la variable.
std::uintptr_t ResolveRipRelative(std::uintptr_t displacementAddress, std::uintptr_t nextInstruction);

}  // namespace luma::sdk
