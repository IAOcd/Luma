// =============================================================================
//  watermark.h - Módulo "Marca de agua": muestra "Luma" en una esquina
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::hud {

class Watermark : public Module {
public:
    Watermark();

    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    void OnRenderHud() override;

    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int corner_ = 0;  // 0 = arriba izq., 1 = arriba der., 2 = abajo izq., 3 = abajo der.
    bool showVersion_ = true;
    float scale_ = 1.3f;
};

}  // namespace luma::modules::hud
