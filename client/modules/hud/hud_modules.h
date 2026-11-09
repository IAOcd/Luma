// =============================================================================
//  hud_modules.h - Los módulos del HUD que NO leen memoria del juego (Fase 5)
// =============================================================================
//  FPS, CPS, Keystrokes y Reloj. Todos usan el estilo común del HUD
//  (render/hud.h), se pueden mover y escalar con el editor de HUD y tener su
//  propio estilo (render/module_options.h).
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
    int format_ = 0;        // 0 = "60 FPS", 1 = "FPS: 60", 2 = "60"
    int rate_ = 1;          // actualización: 0 = rápida, 1 = normal, 2 = lenta
    bool showMs_ = false;   // agregar el frametime
    bool colorize_ = false; // verde/amarillo/rojo según los FPS
    double shown_ = 0.0;
    double shownMs_ = 0.0;
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
    int format_ = 0;         // 0 = "3 CPS", 1 = "CPS: 3"
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
    bool showCps_ = true;     // los botones del mouse muestran los CPS al hacer clic
    int keySize_ = 18;        // lado de una tecla, en píxeles de HUD
    int gap_ = 2;             // separación
    int animation_ = 1;       // 0 = instantánea, 1 = rápida, 2 = suave
    int mouseLabels_ = 0;     // 0 = "LMB / RMB", 1 = "Izq. / Der."
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
    int format_ = 0;      // 0 = 24 h, 1 = 12 h
    bool seconds_ = false;
    int date_ = 0;        // 0 = sin fecha, 1 = "09/11", 2 = "09/11/2026"
};

}  // namespace luma::modules::hud
