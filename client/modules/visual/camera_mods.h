// =============================================================================
//  camera_mods.h - Fullbright y Freelook (Fase 8)
// =============================================================================
//  - Fullbright: ver todo iluminado, como con visión nocturna pero sin el
//    efecto azul. Solo cambia el brillo con que se DIBUJA (el "gamma" que
//    pide el juego a sus opciones): la luz real del mundo no cambia.
//  - Freelook (perspectiva libre): mirar alrededor (en tercera persona) sin
//    girar al jugador. Al desactivarlo, la cámara vuelve detrás tuyo.
//
//  Usan sdk/camera.h.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::visual {

class Fullbright : public Module {
public:
    Fullbright();
    const char* IconSvg() const override;
    bool SdkReady() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    float gamma_ = 10.0f;
};

class Freelook : public Module {
public:
    Freelook();
    bool QuietKey() const override { return true; }
    bool HasTick() const override { return true; }
    void OnTick() override;
    const char* IconSvg() const override;
    bool SdkReady() const override;
    bool NeedsPlayer() const override { return true; }
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int view_ = 1;  // 1 = tercera persona detrás, 2 = delante
    int mode_ = 0;  // 0 = mantener la tecla, 1 = alternar (pulsar para activar y otra vez para desactivar)
};

class Zoom : public Module {
public:
    Zoom();
    const char* IconSvg() const override;
    bool SdkReady() const override;
    bool QuietKey() const override { return true; }
    bool HasTick() const override { return true; }
    void OnTick() override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Apply() const;
    int mode_ = 0;           // 0 = mantener la tecla, 1 = alternar
    float factor_ = 4.0f;    // aumento (4 = 4 veces más cerca)
    float current_ = 4.0f;   // aumento actual (cambia con la rueda)
    bool wheel_ = true;      // la rueda acerca y aleja mientras haces zoom
    bool keepWheel_ = false; // recordar el aumento elegido con la rueda
    bool smooth_ = true;
    float smoothSpeed_ = 12.0f;
    bool lowerSensitivity_ = true;  // mover la cámara más lento con zoom
};

class FovChanger : public Module {
public:
    FovChanger();
    const char* IconSvg() const override;
    bool SdkReady() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    float degrees_ = 90.0f;
};

}  // namespace luma::modules::visual
