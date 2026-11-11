// =============================================================================
//  players.h - La lista de jugadores del mundo y la dirección del servidor
// =============================================================================
//  Para el módulo de lista de jugadores (Tab). Todo se lee de la memoria del
//  juego con lecturas seguras (1.26.6x; la idea de dónde está viene del client
//  open source Latite, el código es propio):
//
//  LISTA DE JUGADORES (la misma que usa el juego para su menú de pausa):
//      cliente +0x1D0 -> Minecraft +0xD0 -> GameSession
//      GameSession: +0x28 = 1 si hay mundo; +0x40 = Level
//      Level +0x4E8 -> std::unordered_map<UUID, PlayerListEntry>
//      PlayerListEntry: +0x18 nombre (std::string), +0xA0 skin (como la tuya)
//
//  DIRECCIÓN DEL SERVIDOR:
//      jugador +0x820 -> PacketSender +0x20 -> NetworkSystem +0x120 ->
//      RemoteConnectorComposite (+0x90 NetherNet, +0x98 RakNet) -> su función
//      virtual n.º 3 -> GameConnectionInfo: +0x8 IP, +0x28 dirección escrita
//      (por ejemplo "play.servidor.net"), +0x8C puerto.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::sdk::players {

struct Player {
    std::string name;                // sin los códigos de color de Minecraft (§)
    std::string skinId;              // para saber si su skin cambió
    std::uint8_t face[8 * 8 * 4]{};  // su cara de 8x8 (RGBA), ya con la segunda capa
    bool hasFace = false;
};

// Lee la lista (hasta "max" jugadores). "withFaces": también las caras (más
// lento: hacerlo solo para los jugadores nuevos). false si no hay mundo.
bool Read(std::vector<Player>& out, bool withFaces, std::size_t max = 200);

struct Server {
    std::string address;  // la dirección escrita (o la IP)
    int port = 0;
};
// false en un mundo local (o si no se pudo leer).
bool ReadServer(Server& out);
// El identificador (UUID, 16 bytes) de un jugador de la lista, por su nombre.
bool UuidOf(const std::string& name, std::uint8_t out[16]);

// El nombre del mundo (en un mundo local; en un servidor es el que puso el servidor).
bool WorldName(std::string& out);

}  // namespace luma::sdk::players
