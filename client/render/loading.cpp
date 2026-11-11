// =============================================================================
//  loading.cpp - Implementación de la pantalla de carga
// =============================================================================
#include "render/loading.h"

#include <imgui.h>
#include <imgui_internal.h>  // ImLerp

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/version.h"
#include "perf/timer.h"
#include "render/assets.h"
#include "render/glass.h"
#include "render/theme.h"

namespace luma::render::loading {

namespace {

constexpr double kFadeInMs = 250.0;
constexpr double kFadeOutMs = 450.0;
constexpr double kMinVisibleMs = 1200.0;  // que no sea un parpadeo si todo carga rápido

std::mutex g_mutex;
std::string g_step = "Iniciando...";
float g_targetProgress = 0.0f;
float g_shownProgress = 0.0f;  // animado hacia g_targetProgress
std::int64_t g_beganAt = 0;
std::int64_t g_finishRequestedAt = 0;
std::int64_t g_fadeStartedAt = 0;
std::atomic<bool> g_visible{false};
bool g_finishing = false;

double MsSince(std::int64_t ticks) {
    return perf::TicksToMs(perf::Now() - ticks);
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

}  // namespace

void Begin() {
    std::lock_guard lock(g_mutex);
    g_step = "Iniciando...";
    g_targetProgress = 0.0f;
    g_shownProgress = 0.0f;
    g_beganAt = perf::Now();
    g_finishing = false;
    g_fadeStartedAt = 0;
    g_visible.store(true);
}

void SetStep(const char* text, float progress) {
    std::lock_guard lock(g_mutex);
    g_step = text;
    g_targetProgress = std::clamp(progress, 0.0f, 1.0f);
}

void Finish() {
    std::lock_guard lock(g_mutex);
    g_step = "Done";
    g_targetProgress = 1.0f;
    g_finishing = true;
    g_finishRequestedAt = perf::Now();
}

bool IsVisible() {
    return g_visible.load(std::memory_order_relaxed);
}

void Render() {
    std::lock_guard lock(g_mutex);
    if (!g_visible.load()) {
        return;
    }

    // --- Transparencia general (aparecer y desaparecer) --------------------------
    float alpha = EaseOut(static_cast<float>(MsSince(g_beganAt) / kFadeInMs));
    if (g_finishing) {
        // Esperar el tiempo mínimo y a que la barra llegue al final.
        if (g_fadeStartedAt == 0 && MsSince(g_beganAt) >= kMinVisibleMs && g_shownProgress > 0.995f) {
            g_fadeStartedAt = perf::Now();
        }
        if (g_fadeStartedAt != 0) {
            const float t = static_cast<float>(MsSince(g_fadeStartedAt) / kFadeOutMs);
            if (t >= 1.0f) {
                g_visible.store(false);
                return;
            }
            alpha *= 1.0f - t * t;
        }
    }
    g_shownProgress = ImLerp(g_shownProgress, g_targetProgress,
                             std::min(1.0f, ImGui::GetIO().DeltaTime * 6.0f));

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const theme::Fonts& fonts = theme::GetFonts();
    const float fontSize = ImGui::GetFontSize();
    auto col = [alpha](ImVec4 c) {
        c.w *= alpha;
        return ImGui::ColorConvertFloat4ToU32(c);
    };

    // Fondo levemente oscurecido.
    draw->AddRectFilled(ImVec2(0, 0), display, col(ImVec4(0.02f, 0.02f, 0.04f, 0.35f)));

    // Tarjeta centrada (sube un poco al aparecer).
    const float width = fontSize * 22.0f;
    const float height = fontSize * 11.0f;
    const float rise = (1.0f - alpha) * fontSize;
    const ImVec2 min((display.x - width) * 0.5f, (display.y - height) * 0.5f + rise);
    const ImVec2 max(min.x + width, min.y + height);
    const float rounding = theme::Get().rounding * theme::UiScale() * 1.2f;
    const theme::Palette& p = theme::P();
    // Tarjeta de vidrio (sin desenfoque: con el menú cerrado no se desenfoca
    // el juego, así que el vidrio lleva más tinte).
    glass::Panel(draw, min, max, rounding, alpha);

    // Logo de Luma que "respira": crece y se achica muy levemente.
    const float time = static_cast<float>(MsSince(g_beganAt) / 1000.0);
    const ImVec2 center(min.x + width * 0.5f, min.y + height * 0.30f);
    const float breathe = 1.0f + 0.04f * std::sin(time * 3.0f);
    const float logoSize = fontSize * 3.0f * breathe;
    assets::DrawLogo(draw, center, logoSize, col(p.text));
    const ImU32 accent = col(theme::Accent(1.0f));
    const float radius = fontSize * 1.3f;

    // Título y paso actual.
    ImFont* title = fonts.title != nullptr ? fonts.title : ImGui::GetFont();
    const ImVec2 titleSize = title->CalcTextSizeA(title->FontSize, FLT_MAX, 0.0f, kName);
    draw->AddText(title, title->FontSize, ImVec2(center.x - titleSize.x * 0.5f, center.y + radius * 1.15f),
                  col(p.text), kName);
    const ImVec2 stepSize = ImGui::CalcTextSize(g_step.c_str());
    const float stepY = center.y + radius * 1.15f + titleSize.y + fontSize * 0.35f;
    draw->AddText(ImVec2(center.x - stepSize.x * 0.5f, stepY), col(p.textDim), g_step.c_str());

    // Barra de progreso con un brillo que la recorre.
    const float barWidth = width * 0.72f;
    const float barHeight = std::max(4.0f, fontSize * 0.28f);
    const ImVec2 barMin(center.x - barWidth * 0.5f, max.y - fontSize * 1.6f);
    const ImVec2 barMax(barMin.x + barWidth, barMin.y + barHeight);
    draw->AddRectFilled(barMin, barMax, col(p.trackOff), barHeight);
    const float fillX = barMin.x + barWidth * g_shownProgress;
    if (fillX > barMin.x + 1.0f) {
        draw->AddRectFilled(barMin, ImVec2(fillX, barMax.y), accent, barHeight);
        const float shine = barMin.x + std::fmod(time * 0.9f, 1.0f) * barWidth;
        if (shine < fillX) {
            draw->AddRectFilled(ImVec2(std::max(barMin.x, shine - barWidth * 0.06f), barMin.y),
                                ImVec2(std::min(fillX, shine + barWidth * 0.06f), barMax.y),
                                col(ImVec4(1, 1, 1, 0.35f)), barHeight);
        }
    }
    char percent[32];
    std::snprintf(percent, sizeof(percent), "%d%%  ·  %s", static_cast<int>(g_shownProgress * 100.0f + 0.5f),
                  kVersion);
    const ImVec2 percentSize = ImGui::CalcTextSize(percent);
    draw->AddText(ImVec2(center.x - percentSize.x * 0.5f, barMax.y + fontSize * 0.3f), col(p.textDim), percent);
}

}  // namespace luma::render::loading
