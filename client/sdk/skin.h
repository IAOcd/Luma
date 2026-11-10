// =============================================================================
//  skin.h - Leer la skin de tu jugador desde el juego (para el launcher)
// =============================================================================
//  Minecraft Bedrock no guarda tu skin como archivo en la PC (las skins del
//  Vestidor y del Marketplace vienen de la nube). Pero mientras juegas, la
//  imagen de la skin está en la memoria del juego: es la "SerializedSkin" del
//  jugador, que el juego también usa para mandar tu skin a los servidores.
//
//  Dónde está (1.26.6x; la idea viene del client open source Latite, el
//  código es propio):
//      Player +0xAD8 -> SerializedSkinRef -> (+0x0) la skin de verdad:
//          +0x00 std::string id, +0x20 std::string nombre,
//          +0x80 SkinImage { formato, ancho, alto, profundidad, uso, relleno,
//                            { borrador, puntero a los píxeles RGBA, tamaño } }
//
//  Todo con lecturas seguras: si algo no cuadra (otra versión del juego),
//  no se lee nada y no pasa nada.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::sdk::skin {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4 bytes
    std::string id;                  // identificador de la skin (para saber si cambió)
};

// La skin del jugador local, o false si no hay jugador o no se pudo leer.
bool ReadLocal(Image& out);

}  // namespace luma::sdk::skin
