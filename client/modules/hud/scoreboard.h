// =============================================================================
//  scoreboard.h - Módulo "Scoreboard movible"
// =============================================================================
//  El scoreboard (la tabla de puntos de la derecha que muestran muchos
//  servidores y minijuegos) lo dibuja el juego. Este módulo lo deja MOVER con
//  el editor de HUD, como cualquier elemento de Luma. Ver sdk/hud_ui.h.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::hud {

class MovableScoreboard : public Module {
public:
    MovableScoreboard();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool CanScale() const override { return false; }  // el texto lo dibuja el juego
    bool SdkReady() const override;
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;

private:
    float lastX_ = -1.0f;
    float lastY_ = -1.0f;
};

}  // namespace luma::modules::hud
