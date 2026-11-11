// =============================================================================
//  perf_overlay.h - Módulo "Overlay de rendimiento"
// =============================================================================
//  Una caja con el frametime, los FPS, el costo de Luma por frame y un
//  gráfico de los últimos frames. Es el "overlay de debug" que pide el
//  proyecto para medir siempre cuánto cuesta Luma.
// =============================================================================
#pragma once

#include <cstdint>

#include "modules/module.h"

namespace luma::modules::performance {

class PerfOverlay : public Module {
public:
    PerfOverlay();

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
    bool showGraph_ = true;
    bool showLumaCost_ = true;
    bool showGpu_ = true;   // GPU busy time per frame (perf/gpu_timer.h)
    int graphHeight_ = 20;  // alto del gráfico, en píxeles de HUD
    int graphFrames_ = 120; // cuántos frames muestra
    // Valores mostrados (se actualizan 4 veces por segundo para poder leerlos).
    double fps_ = 0.0;
    double frameMs_ = 0.0;
    double costMs_ = 0.0;
    double gpuMs_ = -1.0;   // negative = no measurement yet
    std::int64_t lastUpdate_ = 0;
};

}  // namespace luma::modules::performance
