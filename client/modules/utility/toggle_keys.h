// =============================================================================
//  toggle_keys.h - Módulos "Toggle sprint" y "Toggle sneak"
// =============================================================================
//  En vez de mantener apretado Ctrl (correr) o Shift (agacharse), se
//  "alternan": Luma mantiene la tecla apretada por ti (ver render/input.h,
//  "teclas sostenidas"). Pulsar la tecla de verdad lo alterna.
//
//  No lee ni cambia memoria del juego: solo le manda los mismos mensajes de
//  teclado que mandaría Windows si la tuvieras apretada. Es la misma función
//  que el juego ofrece en sus opciones para agacharse ("alternar"), así que
//  no da ninguna ventaja.
//
//  Opcional: un texto en el HUD ("[Corriendo (alternado)]").
// =============================================================================
#pragma once

#include "modules/module.h"
#include "render/input.h"

namespace luma::modules::utility {

class ToggleKey : public Module {
public:
    // slot: Sprint o Sneak. Cada uno tiene su id, nombre y tecla por defecto.
    explicit ToggleKey(render::input::HoldSlot slot);

    const char* IconSvg() const override;
    bool HasHud() const override { return showText_; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int GameKey() const;

    render::input::HoldSlot slot_;
    int keyIndex_ = 0;        // posición en la lista de teclas (ver .cpp)
    bool showText_ = true;
    char label_[48] = "";  // texto propio para el HUD (vacío = el de fábrica)
};

}  // namespace luma::modules::utility
