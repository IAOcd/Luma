// =============================================================================
//  perf_overlay.cpp - Implementación del overlay de rendimiento
// =============================================================================
#include "modules/performance/perf_overlay.h"

#include <imgui.h>

#include <algorithm>

#include "perf/frame_stats.h"
#include "render/theme.h"

namespace luma::modules::performance {

namespace {

// Promedios de los últimos N frames del historial.
constexpr std::size_t kAverageFrames = 60;

}  // namespace

PerfOverlay::PerfOverlay()
    : Module("perf_overlay", "Overlay de rendimiento",
             "Muestra FPS, frametime y el costo de Luma por frame en pantalla.",
             Category::Performance) {}

void PerfOverlay::OnRenderHud() {
    const perf::History& history = perf::GetHistory();
    if (history.count == 0) {
        return;
    }

    // Promedio de los últimos kAverageFrames frames (los más recientes están
    // justo antes de "next" en el ring buffer).
    const std::size_t n = std::min(kAverageFrames, history.count);
    double frameSum = 0.0;
    double costSum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t index = (history.next + perf::kHistorySize - 1 - i) % perf::kHistorySize;
        frameSum += history.frameMs[index];
        costSum += history.costUs[index];
    }
    const double frameMs = frameSum / static_cast<double>(n);
    const double costMs = costSum / static_cast<double>(n) / 1000.0;
    const double fps = frameMs > 0.0 ? 1000.0 / frameMs : 0.0;

    // Ventana pequeña sin bordes ni título, que no recibe clics.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = ImGui::GetFontSize() * 0.6f;
    const bool right = corner_ == 1 || corner_ == 3;
    const bool bottom = corner_ == 2 || corner_ == 3;
    ImGui::SetNextWindowPos(ImVec2(right ? display.x - margin : margin,
                                   bottom ? display.y - margin : margin),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##luma_perf_overlay", nullptr, kFlags)) {
        ImGui::TextColored(theme::Accent(1.0f), "%.0f FPS", fps);
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f ms", frameMs);
        if (showLumaCost_) {
            ImGui::TextDisabled("Luma: %.3f ms/frame", costMs);
        }
        if (showGraph_) {
            const int offset =
                history.count == perf::kHistorySize ? static_cast<int>(history.next) : 0;
            ImGui::PlotLines("##ft", history.frameMs.data(), static_cast<int>(history.count),
                             offset, nullptr, 0.0f, 33.3f,
                             ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetFontSize() * 2.2f));
        }
    }
    ImGui::End();
}

bool PerfOverlay::DrawSettings() {
    bool changed = false;
    const char* corners[] = {"Arriba a la izquierda", "Arriba a la derecha", "Abajo a la izquierda",
                             "Abajo a la derecha"};
    changed |= ImGui::Combo("Esquina", &corner_, corners, IM_ARRAYSIZE(corners));
    changed |= ImGui::Checkbox("Gráfico de frametime", &showGraph_);
    changed |= ImGui::Checkbox("Mostrar costo de Luma", &showLumaCost_);
    ImGui::TextDisabled("Con este módulo activo Luma dibuja cada frame, así que su costo sube.");
    return changed;
}

void PerfOverlay::SaveSettings(nlohmann::json& out) const {
    out["corner"] = corner_;
    out["showGraph"] = showGraph_;
    out["showLumaCost"] = showLumaCost_;
}

void PerfOverlay::LoadSettings(const nlohmann::json& in) {
    corner_ = std::clamp(in.value("corner", 1), 0, 3);
    showGraph_ = in.value("showGraph", true);
    showLumaCost_ = in.value("showLumaCost", true);
}

}  // namespace luma::modules::performance
