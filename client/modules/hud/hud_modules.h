// =============================================================================
//  hud_modules.h - Los módulos del HUD que NO leen memoria del juego (Fase 5)
// =============================================================================
//  FPS, CPS, Keystrokes y Reloj. Todos usan el estilo común del HUD
//  (render/hud.h) y se pueden mover y escalar con el editor de HUD.
//
//  De dónde sale cada dato (todo funciones de Windows o de Luma, estables):
//    - FPS: la medición de frames de Luma (perf/frame_stats.h).
//    - CPS: los clics de la cola del mouse del juego (core/clicks.h).
//    - Keystrokes: GetAsyncKeyState, "¿está apretada esta tecla?".
//    - Reloj: la hora de Windows.
//
//  RENDIMIENTO: el texto se arma en arreglos fijos (snprintf), sin pedir
//  memoria, y los números se actualizan unas veces por segundo (no cada
//  frame) para que no "tiemblen".
// =============================================================================
#pragma once

#include <array>
#include <cstdint>

#include "modules/module.h"

namespace luma::modules::hud {

class FpsCounter : public Module {
public:
    FpsCounter();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int format_ = 0;  // 0 = "60 FPS", 1 = "FPS: 60"
    double shown_ = 0.0;
    std::int64_t lastUpdate_ = 0;
};

class CpsCounter : public Module {
public:
    CpsCounter();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool showRight_ = true;  // "3 | 1 CPS" (izquierdo | derecho)
};

class Keystrokes : public Module {
public:
    Keystrokes();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool showMouse_ = true;
    bool showSpace_ = true;
    bool showCps_ = true;          // los botones del mouse muestran los CPS al hacer clic
    // Animación de cada tecla (0 = suelta, 1 = apretada): W A S D, espacio, izq., der.
    std::array<float, 7> anim_{};
};

class Clock : public Module {
public:
    Clock();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool twelveHour_ = false;
    bool seconds_ = false;
};

}  // namespace luma::modules::hud
