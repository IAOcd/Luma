// =============================================================================
//  freecam.h - Free cam: volar con la cámara mientras tu jugador queda quieto
// =============================================================================
//  SOLO EN TUS MUNDOS (regla de Luma: nada que dé ventaja en un servidor). Es
//  un módulo "solo local": no se puede activar en un servidor y, si entras a
//  uno con él activo, se apaga solo (lo hace modules/module_manager).
//
//  Cómo funciona (los detalles en sdk/camera.h):
//    - El jugador no gira: el mismo truco del freelook (al juego le decimos
//      que la cámara sigue mirando hacia donde mirabas al activarlo).
//    - El jugador no camina: W, A, S, D, espacio, Shift y Ctrl no le llegan
//      al juego (render/input) y los clics tampoco (sdk/mouse_queue), así no
//      rompe ni pone bloques sin querer.
//    - La cámara se mueve sola: en cada frame Luma calcula su posición nueva
//      y se la pone a la cámara del juego SOLO mientras dibuja el mundo.
//    - Al salir, la cámara vuelve exactamente a donde estaba (nunca se movió
//      para la lógica del juego).
//
//  Controles: WASD (hacia donde miras), espacio sube, Shift baja, Ctrl más
//  rápido, rueda del mouse cambia la velocidad.
// =============================================================================
#pragma once

#include <cstdint>

#include "modules/module.h"

namespace luma::modules::world {

class FreeCam : public Module {
public:
    FreeCam();
    const char* IconSvg() const override;
    bool SdkReady() const override;
    bool NeedsPlayer() const override { return true; }
    bool QuietKey() const override { return true; }
    bool HasTick() const override { return true; }
    bool HasHud() const override { return true; }           // el cartelito de la velocidad
    bool IsHudElement() const override { return false; }    // (siempre arriba al centro)
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnTick() override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void ApplyInput() const;
    int mode_ = 1;            // 0 = mantener la tecla, 1 = alternar
    float speed_ = 10.0f;     // bloques por segundo
    float smoothing_ = 6.0f;  // 0 = sin suavizado
    bool fullLook_ = true;    // W va hacia donde miras, también arriba/abajo
    bool showBody_ = true;    // tercera persona: ver a tu jugador quieto
    bool failed_ = false;     // no se pudo arrancar (sin jugador): apagar en el próximo tick
    bool playing_ = false;    // ¿jugando? (sin menú, chat ni inventario)
    Module* zoom_ = nullptr;  // mientras el zoom está activo, la rueda es suya
    bool zoomHadWheel_ = false;
    std::uint64_t shownUntil_ = 0;  // hasta cuándo se muestra la velocidad (GetTickCount64)
};

}  // namespace luma::modules::world
