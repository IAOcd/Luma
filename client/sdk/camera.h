// =============================================================================
//  camera.h - Cámara y opciones de video del juego (Fase 8)
// =============================================================================
//  Hooks a funciones del juego (firmas propias; la idea de cuáles usar viene
//  de estudiar el client open source Latite):
//
//    - Options::getGamma: el juego le pregunta a sus opciones "¿cuánto
//      brillo?". Si Fullbright está activo, Luma responde un valor alto: el
//      mundo se ve iluminado aunque sea de noche o en una cueva. No cambia
//      el mundo ni la luz real (los mobs aparecen igual).
//    - Options::getPerspective: "¿primera o tercera persona?". El freelook
//      fuerza la tercera persona mientras está activo.
//    - UpdatePlayerFromCameraSystemUtil::_updatePlayer: el juego copia hacia
//      dónde mira la cámara al jugador. Con el freelook, Luma le hace creer
//      que la cámara sigue mirando hacia donde mirabas al activarlo: la
//      cámara gira, el jugador no.
//    - LevelRenderer::renderLevel: dibuja el mundo (para el zoom y el FOV).
//
//  Si una firma no se encuentra, solo se desactiva lo que la usa.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::camera {

bool Init();

bool GammaAvailable();
bool PerspectiveAvailable();
bool CameraAvailable();
bool RenderAvailable();  // renderLevel (zoom y FOV)

// Brillo forzado (Fullbright). Negativo = sin cambiar (el del juego).
void SetGammaOverride(float gamma);

// Perspectiva forzada: 0 = primera persona, 1 = tercera detrás, 2 = tercera
// delante. -1 = la que eligió el jugador.
void SetPerspectiveOverride(int perspective);

// Multiplicador del campo de visión (zoom): 1 = el del juego, 0.25 = 4x de zoom.
// Se aplica justo antes de que el juego dibuje el mundo, sobre el FOV que
// calculó en ese frame (así nunca se acumula).
void SetFovMultiplier(float multiplier);
// FOV fijo en grados (FOV personalizado). 0 = el del juego.
void SetFovOverride(float degrees);
// Qué tan rápido llega el zoom a su valor (0 = de golpe; 10 = suave).
void SetZoomSmoothing(float speed);
// El multiplicador de FOV de ESTE momento (animado): 1 = sin zoom, 0.25 = 4x.
float CurrentFovMultiplier();

// Ocultar la mano del jugador (true) o dejar la opción del juego (false).
void SetHideHand(bool hide);
bool HideHandAvailable();

// Freelook: mientras está activo, el jugador queda mirando hacia (pitch, yaw).
void SetFreelook(bool active, float pitch, float yaw);

void BeginUnload();

}  // namespace luma::sdk::camera
