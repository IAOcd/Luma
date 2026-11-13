// =============================================================================
//  cape.h - Capas de Luma: ponerle a tu jugador una capa de Luma
// =============================================================================
//  DÓNDE ESTÁ LA CAPA (1.26.63, lo encontramos leyendo la skin en memoria):
//  la capa es parte de la skin del jugador, la "SerializedSkin":
//      +0x00 id de la skin              (std::string; el juego nombra las texturas con él)
//      +0x20 id "completo" = skin + capa
//      +0x80 imagen de la skin          (64x64...)
//      +0xB0 imagen de la capa          (64x32; vacía si no tienes)
//      +0x158 id de la capa             (std::string)
//
//  LO QUE APRENDIMOS (a los golpes):
//    - El juego COMPARTE las skins (std::shared_ptr): no las copia. Cambiar
//      la skin "en el lugar" no alcanza: el juego solo vuelve a armar las
//      texturas cuando un jugador recibe un objeto de skin NUEVO.
//    - En un mundo local hay dos "tú": el del servidor (el que encuentra
//      sdk/player) y el del cliente (el que se dibuja).
//    - Copiar una skin a mano no es seguro (tiene partes que solo el juego
//      sabe copiar). Hay que lograr que el juego haga la copia.
//
//  CÓMO LO HACEMOS (paso 1: mundos locales):
//  Igual que "skin al instante" (sdk/skin_apply): armamos un paquete de skin
//  (PlayerSkinPacket) con tu skin y la capa de Luma y se lo mandamos a tu
//  servidor (en un mundo local, tu propio juego). El servidor lo lee (así
//  arma una COPIA nueva), te la pone y se la reenvía a tu cliente, que vuelve
//  a armar las texturas. Así ya cambia la skin al instante.
//  Detalle honesto: si alguien entra a tu mundo local, también la ve.
//
//  En servidores (paso 2, todavía no): ahí NO se manda nada al servidor (lo
//  verían todos). Habrá que hacer la copia del lado del cliente.
//
//  Si algo no se encuentra (otra versión del juego), Available() es false y
//  las capas no hacen nada. Nunca crashear: lecturas seguras y __try.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::sdk::cape {

bool Init();
bool Available();

// Ancho y alto de la imagen de una capa normal. También sirven capas "HD"
// con la misma forma (ancho = 2 x alto, múltiplo de 64): 128x64, 256x128...
inline constexpr unsigned kWidth = 64;
inline constexpr unsigned kHeight = 32;
inline bool ValidSize(unsigned w, unsigned h) {
    return w >= 64 && w <= 1024 && w % 64 == 0 && w == h * 2;
}

// TU capa: la que quieres llevar ("rgba" de 64x32 e id de la capa; los dos
// vacíos = "ninguna": vuelve la capa que tenías en el juego). Se aplica en
// el próximo frame de la interfaz del juego (OnUiFrame).
void SetLocalCape(const std::vector<std::uint8_t>& rgba, unsigned width, unsigned height, const std::string& capeId);

// Hilo de la interfaz del juego, antes de dibujar (lo llama el mismo hook que
// skinapply::OnUiFrame): si hay una capa pendiente, la manda.
void OnUiFrame();

}  // namespace luma::sdk::cape
