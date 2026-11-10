// =============================================================================
//  paperdoll.h - Módulo "Muñeco movible": el muñequito de tu personaje del HUD
// =============================================================================
//  El juego ya dibuja un muñeco de tu personaje (arriba a la izquierda) cuando
//  corres, te agachas, nadas o vuelas. Este módulo lo deja MOVER y AGRANDAR
//  con el editor de HUD como cualquier otro elemento, y opcionalmente lo deja
//  siempre visible. El dibujo lo sigue haciendo el juego (ver sdk/paperdoll.h).
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::hud {

class MovablePaperdoll : public Module {
public:
    MovablePaperdoll();

    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool SdkReady() const override;
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;

    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Push(bool enabled, float x, float y, float scale) const;
    bool alwaysShow_ = true;
    float lastX_ = 0.0f;
    float lastY_ = 0.0f;
    float lastScale_ = 1.0f;
};

}  // namespace luma::modules::hud
