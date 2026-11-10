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
    void ApplyStyle(int style);  // carga los valores de un estilo (Normal, Cine, Catalejo)
    int style_ = 0;             // 0 = normal, 1 = cinematográfico, 2 = catalejo
    int mode_ = 0;              // 0 = mantener la tecla, 1 = alternar
    float factor_ = 4.0f;       // aumento (4 = 4 veces más cerca)
    float current_ = 4.0f;      // aumento actual (cambia con la rueda)
    bool wheel_ = true;         // la rueda acerca y aleja mientras haces zoom
    bool keepWheel_ = false;    // recordar el aumento elegido con la rueda
    bool smooth_ = true;
    float smoothSpeed_ = 12.0f;
    bool followSensitivity_ = true;  // la sensibilidad sigue al zoom
    float followStrength_ = 1.0f;
    bool bars_ = false;          // barras de cine
    float barsHeight_ = 0.11f;   // alto de cada barra (fracción de la pantalla)
    bool spyglass_ = false;      // círculo de catalejo
    bool hideHud_ = false;       // ocultar el HUD de Luma
    bool hideHand_ = false;      // ocultar la mano
    bool hideGameUi_ = false;    // ocultar la interfaz del juego (pulsa F1)
    bool sounds_ = true;
    bool wheelSounds_ = true;
    bool tappedF1_ = false;      // ¿pulsamos F1 al empezar? (para volver a pulsarlo al terminar)
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
