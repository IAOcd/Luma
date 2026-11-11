// =============================================================================
//  perf_overlay.cpp - Implementación del overlay de rendimiento
// =============================================================================
#include "modules/performance/perf_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/hud.h"
#include "render/icons.h"
#include "render/ui.h"

namespace luma::modules::performance {

namespace rh = render::hud;
namespace ui = render::ui;

namespace {

constexpr std::size_t kGraphFrames = 240;  // máximo de frames que muestra el gráfico

}  // namespace

PerfOverlay::PerfOverlay()
    : Module("perf_overlay", "Performance overlay",
             "Shows FPS, frametime, Luma's cost per frame and a graph of the latest frames.",
             Category::Performance) {
    // Arriba a la derecha.
    HudLayout l;
    l.x = 0.996f;
    l.y = 0.007f;
    l.anchorX = 1.0f;
    SetDefaultLayout(l);
}

const char* PerfOverlay::IconSvg() const {
    return render::icons::kChart;
}

ImVec2 PerfOverlay::OnRenderHud(const HudFrame& frame) {
    const perf::History& history = perf::GetHistory();
    const std::int64_t now = perf::Now();
    if (lastUpdate_ == 0 || perf::TicksToMs(now - lastUpdate_) >= 250.0) {
        lastUpdate_ = now;
        const perf::Recent recent = perf::RecentAverage(60);
        frameMs_ = recent.frameMs;
        fps_ = recent.frameMs > 0.0 ? 1000.0 / recent.frameMs : 0.0;
        costMs_ = recent.costUs / 1000.0;
    }

    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float pad = 5.0f * u;
    const float line = m.capHeight + 4.0f * u;  // alto de cada renglón
    char lines[2][48];
    std::snprintf(lines[0], sizeof(lines[0]), "%.0f FPS  %.2f ms", fps_, frameMs_);
    std::snprintf(lines[1], sizeof(lines[1]), "Luma: %.3f ms", costMs_);
    const int count = showLumaCost_ ? 2 : 1;

    float width = 70.0f * u;
    for (int i = 0; i < count; ++i) {
        width = std::max(width, rh::TextSize(m, lines[i]).x + pad * 2.0f);
    }
    width = std::floor(width);
    const float graphH = showGraph_ ? static_cast<float>(graphHeight_) * u : 0.0f;
    const float height = std::floor(pad * 2.0f + line * static_cast<float>(count) - 4.0f * u +
                                    (showGraph_ ? graphH + 4.0f * u : 0.0f));
    const ImVec2 o = frame.pos;
    ImDrawList* draw = frame.draw;
    rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));

    float y = o.y + pad;
    for (int i = 0; i < count; ++i) {
        // La segunda línea, un poco más tenue.
        rh::Text(draw, m, ImVec2(o.x + pad, y - m.capTop), lines[i], rh::TextColor(i == 0 ? 1.0f : 0.7f));
        y += line;
    }

    // Gráfico: una línea con el frametime de los últimos frames (más alto =
    // más lento). Los puntos van en un arreglo fijo: sin pedir memoria.
    if (showGraph_ && history.count > 1) {
        const float gx = o.x + pad;
        const float gy = y;
        const float gw = width - pad * 2.0f;
        draw->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + graphH), IM_COL32(255, 255, 255, 14));
        const std::size_t n = std::min({kGraphFrames, history.count, static_cast<std::size_t>(graphFrames_)});
        float peak = 33.3f;
        for (std::size_t i = 0; i < n; ++i) {
            peak = std::max(peak, history.frameMs[(history.next + perf::kHistorySize - n + i) % perf::kHistorySize]);
        }
        std::array<ImVec2, kGraphFrames> points{};
        for (std::size_t i = 0; i < n; ++i) {
            const float ms = history.frameMs[(history.next + perf::kHistorySize - n + i) % perf::kHistorySize];
            const float px = gx + gw * static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(1, n - 1));
            const float py = gy + graphH - std::min(1.0f, ms / peak) * graphH;
            points[i] = ImVec2(px, py);
        }
        draw->AddPolyline(points.data(), static_cast<int>(n), rh::TextColor(0.9f), ImDrawFlags_None,
                          std::max(1.0f, u * 0.75f));
    }
    return ImVec2(width, height);
}

bool PerfOverlay::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Luma's cost", &showLumaCost_, "(ms por frame)");
    changed |= ui::Toggle("Frametime graph", &showGraph_);
    if (showGraph_) {
        changed |= ui::SliderInt("Graph height", &graphHeight_, 10, 50, "%d");
        changed |= ui::SliderInt("Frames in the graph", &graphFrames_, 30, 240, "%d");
    }
    ui::Hint("With a HUD element on, Luma draws every frame and its cost goes up a little.");
    return changed;
}

void PerfOverlay::SaveSettings(nlohmann::json& out) const {
    out["showGraph"] = showGraph_;
    out["showLumaCost"] = showLumaCost_;
    out["graphHeight"] = graphHeight_;
    out["graphFrames"] = graphFrames_;
}

void PerfOverlay::LoadSettings(const nlohmann::json& in) {
    showGraph_ = in.value("showGraph", true);
    showLumaCost_ = in.value("showLumaCost", true);
    graphHeight_ = std::clamp(in.value("graphHeight", 20), 10, 50);
    graphFrames_ = std::clamp(in.value("graphFrames", 120), 30, 240);
}

}  // namespace luma::modules::performance
