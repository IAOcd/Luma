// =============================================================================
//  player_hud.h - Módulos del HUD con datos del jugador (Fase 7)
// =============================================================================
//  Coordenadas, brújula y velocidad. Leen al jugador local con sdk/player.h
//  (lectura segura). Si no hay jugador (menú principal) no dibujan nada; en
//  el editor de HUD muestran un ejemplo para poder ubicarlos.
// =============================================================================
#pragma once

#include <cstdint>

#include "modules/module.h"

namespace luma::modules::hud {

class Coordinates : public Module {
public:
    Coordinates();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool oneLine_ = false;    // "X Y Z" en una línea en vez de tres
    bool decimals_ = false;
    bool showFacing_ = true;  // "Mirando: Norte (-Z)"
    bool showNether_ = false; // coordenadas equivalentes en el Nether (/8)
};

class Compass : public Module {
public:
    Compass();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool showDegrees_ = true;
    int width_ = 140;  // ancho de la tira, en píxeles de HUD
};

class SpeedDisplay : public Module {
public:
    SpeedDisplay();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool vertical_ = false;  // incluir la velocidad vertical (3D)
    int unit_ = 0;            // 0 = m/s, 1 = km/h
    float lastPos_[3]{};
    std::int64_t lastSample_ = 0;
    float shown_ = 0.0f;
};

class PingDisplay : public Module {
public:
    PingDisplay();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsNetwork() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool showInLocal_ = false;  // mostrar "Mundo local" cuando no hay ping
    bool colorize_ = true;      // verde / amarillo / rojo según el ping
};

}  // namespace luma::modules::hud
