// =============================================================================
//  player.h - El jugador local (Fase 6)
// =============================================================================
//  CÓMO ENCONTRAMOS AL JUGADOR (sin direcciones fijas):
//
//  El juego tiene una función "LocalPlayer::applyTurnDelta": la que gira la
//  cámara del jugador cuando mueves el mouse. Su primer parámetro es el
//  propio jugador local. La encontramos por firma (la idea viene de estudiar
//  el client open source Latite; el código es propio) y la hookeamos: cada
//  vez que el juego la llama, Luma se guarda ese puntero.
//
//  Así no hay que adivinar en qué campo de qué objeto vive el jugador: el
//  juego mismo nos lo dice. (Hace falta mover el mouse una vez dentro del
//  mundo para que aparezca.)
//
//  SEGURIDAD:
//    - Toda lectura usa sdk/memory.h (si falla, no crashea).
//    - Al capturarlo guardamos su "vtable" (el primer dato de todo objeto
//      con funciones virtuales). Antes de cada lectura comprobamos que siga
//      siendo la misma: si saliste del mundo y esa memoria se reutilizó para
//      otra cosa, no coincide y no leemos.
//    - La primera vez comprobamos que los offsets tengan sentido (la caja del
//      jugador mide ~0,6 x 1,8). Si no, Available() pasa a false y los
//      módulos del jugador se desactivan con un aviso.
//
//  RENDIMIENTO: solo se lee si algún módulo activo lo pide, y una vez por
//  frame como mucho (los módulos comparten la misma lectura).
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::player {

bool Init();
// ¿Ya se intentó Init? (antes de eso, Available() todavía no dice nada)
bool Initialized();
// ¿Funciona el acceso al jugador? (firma encontrada y offsets válidos)
bool Available();

// El jugador local, o 0 si todavía no se vio.
std::uintptr_t LocalPlayer();

struct Snapshot {
    float eye[3]{};        // posición de los ojos
    float eyeOld[3]{};     // la misma, en el tick anterior (para medir la velocidad exacta)
    float feet[3]{};       // posición de los pies (lo que muestra el juego como coordenadas)
    float velocity[3]{};   // bloques por tick
    float yaw = 0.0f;      // 0 = sur, 90 = oeste, 180 = norte, -90 = este (convención del juego)
    float pitch = 0.0f;    // -90 = arriba, 90 = abajo
    std::int32_t ticks = 0;
};

// Lee al jugador. false si no hay jugador (por ejemplo, en el menú principal).
bool Read(Snapshot& out);

// Multiplicador de sensibilidad de la cámara (1 = sin cambio). Se aplica al
// giro que recibe applyTurnDelta.
void SetSensitivity(float multiplier);
// Otro multiplicador, que usa el zoom (se multiplican entre sí).
void SetZoomSensitivity(float multiplier);
// Sensibilidad que sigue al zoom de ESE momento (animado): con 4x de zoom,
// la cámara gira 4 veces más lento, y cambia suave junto con la animación.
// "strength" 0..1: cuánto la sigue (1 = exactamente).
void SetZoomSensitivityFollow(bool follow, float strength);

void BeginUnload();

}  // namespace luma::sdk::player
