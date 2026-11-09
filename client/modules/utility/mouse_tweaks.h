// =============================================================================
//  mouse_tweaks.h - Ajustes del mouse: rueda, sensibilidad y límite de CPS
// =============================================================================
//  - Desactivar la rueda: mientras juegas, la rueda no cambia de ranura en la
//    barra rápida (en los menús del juego sigue funcionando).
//  - Multiplicador de sensibilidad: multiplica el giro de la cámara. Útil
//    para tener una sensibilidad más baja que el mínimo del juego.
//  - Límite de CPS: los clics por encima de N por segundo se descartan antes
//    de que el juego los vea. Algunos servidores lo piden.
//
//  Ninguno da ventaja: la rueda y el límite QUITAN input; la sensibilidad
//  es lo mismo que el ajuste del juego, con más rango.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::utility {

class ScrollWheelDisabler : public Module {
public:
    ScrollWheelDisabler();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
};

class SensitivityMultiplier : public Module {
public:
    SensitivityMultiplier();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool NeedsPlayer() const override { return true; }
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    float multiplier_ = 0.5f;
};

class CpsCap : public Module {
public:
    CpsCap();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int cap_ = 12;
};

}  // namespace luma::modules::utility
