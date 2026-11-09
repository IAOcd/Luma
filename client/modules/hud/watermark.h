// =============================================================================
//  watermark.h - Módulo "Marca de agua": el logo y "Luma" en pantalla
// =============================================================================
//  El módulo más simple: muestra la marca de Luma. Se mueve y escala con el
//  editor de HUD, como todos.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::hud {

class Watermark : public Module {
public:
    Watermark();

    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;

    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool showVersion_ = true;
    bool box_ = false;  // con caja de fondo, como los demás elementos
    bool showLogo_ = true;
    bool showName_ = true;
};

}  // namespace luma::modules::hud
