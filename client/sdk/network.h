// =============================================================================
//  network.h - Ping y "¿estoy en un servidor o en un mundo local?" (Fase 7)
// =============================================================================
//  CÓMO FUNCIONA:
//  El juego usa RakNet para conectarse a los servidores. Mientras estás
//  conectado, pide periódicamente el ping promedio con la función
//  "RakPeer::GetAveragePing" (la idea viene del client open source Latite;
//  la firma y el código son propios). La hookeamos: cada vez que el juego
//  la llama, nos quedamos con el resultado.
//
//    - Si hace poco (5 s) el juego obtuvo un ping válido -> estás en un
//      servidor (también Realms o el mundo LAN de otra persona).
//    - Si no -> mundo local (o el menú).
//
//  LIMITACIÓN HONESTA: esto se probó en un mundo local. Falta confirmar en
//  un servidor que el juego la llame seguido (si no, Luma podría creer que
//  estás en local estando en un servidor). Mientras tanto, los módulos
//  "solo local" se tratan con precaución: ver OnServer().
// =============================================================================
#pragma once

namespace luma::sdk::network {

bool Init();
bool Available();

// El último ping (ms) que obtuvo el juego, o -1 si no hay.
int PingMs();

// ¿Estás en un servidor? (ping válido en los últimos 5 segundos)
bool OnServer();

void BeginUnload();

}  // namespace luma::sdk::network
