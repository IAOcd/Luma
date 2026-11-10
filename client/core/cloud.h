// =============================================================================
//  cloud.h - Hablar con el servidor de Luma (luma-api.gios-app.workers.dev)
// =============================================================================
//  Para que los usuarios de Luma se reconozcan entre sí:
//    - "Estoy usando Luma": al cargar y cada 5 minutos (si iniciaste sesión
//      con Microsoft en el launcher: el launcher deja tu "pase de Luma" en
//      %APPDATA%\Luma\session.dat, cifrado con DPAPI).
//    - "¿Quién de estos usa Luma?": la lista de jugadores pregunta por los
//      nombres que ve; la respuesta se guarda unos minutos.
//
//  Todo corre en un hilo propio: una conexión lenta nunca traba el juego.
//  Sin internet, simplemente no hay logos.
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace luma::cloud {

void Start();
void Stop();

// Pide (sin esperar) saber cuáles de estos nombres usan Luma. Los que ya se
// preguntaron hace poco no se vuelven a preguntar.
void RequestLookup(const std::vector<std::string>& names);
// ¿Ese jugador usa Luma? (según la última respuesta del servidor)
bool IsLumaUser(const std::string& name);
// ¿Tienes un pase de Luma (iniciaste sesión con Microsoft en el launcher)?
bool SignedIn();

}  // namespace luma::cloud
