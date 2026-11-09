// =============================================================================
//  perf_overlay.h - Módulo "Overlay de rendimiento"
// =============================================================================
//  Muestra en pantalla los FPS, el frametime y cuánto cuesta Luma por frame,
//  con un mini gráfico opcional. Lee los datos de perf/frame_stats.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::performance {

class PerfOverlay : public Module {
public:
    PerfOverlay();

    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    void OnRenderHud() override;

    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int corner_ = 1;  // arriba a la derecha
    bool showGraph_ = true;
    bool showLumaCost_ = true;
};

}  // namespace luma::modules::performance
