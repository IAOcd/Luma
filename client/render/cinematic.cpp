// =============================================================================
//  cinematic.cpp - Barras de cine y catalejo (ver cinematic.h)
// =============================================================================
#include "render/cinematic.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace luma::render::cinematic {

namespace {

std::atomic<float> g_barsTarget{0.0f};
std::atomic<bool> g_spyglassTarget{false};
std::atomic<bool> g_hideHud{false};
// Valores animados (solo el hilo de render los toca).
float g_bars = 0.0f;
float g_spyglass = 0.0f;
std::atomic<bool> g_animating{false};

float Approach(float value, float target, float dt, float speed) {
    value += (target - value) * (1.0f - std::exp(-speed * dt));
    return std::fabs(value - target) < 0.001f ? target : value;
}

}  // namespace

void SetBars(float heightFraction) {
    g_barsTarget.store(std::clamp(heightFraction, 0.0f, 0.3f));
    g_animating.store(true);
}

void SetSpyglass(bool on) {
    g_spyglassTarget.store(on);
    g_animating.store(true);
}

void SetHideHud(bool hide) {
    g_hideHud.store(hide);
}

bool Active() {
    return g_animating.load(std::memory_order_relaxed);
}

bool HideHud() {
    return g_hideHud.load(std::memory_order_relaxed);
}

void Draw() {
    if (!g_animating.load(std::memory_order_relaxed)) {
        return;
    }
    const float dt = std::min(0.1f, ImGui::GetIO().DeltaTime);
    const float barsTarget = g_barsTarget.load();
    const float spyTarget = g_spyglassTarget.load() ? 1.0f : 0.0f;
    g_bars = Approach(g_bars, barsTarget, dt, 9.0f);
    g_spyglass = Approach(g_spyglass, spyTarget, dt, 10.0f);
    if (g_bars == barsTarget && g_spyglass == spyTarget && barsTarget == 0.0f && spyTarget == 0.0f) {
        g_animating.store(false);  // todo terminó de irse: ya no hace falta dibujar
        return;
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 d = ImGui::GetIO().DisplaySize;

    // Catalejo: un círculo transparente en el centro y negro alrededor. Se
    // dibuja como un "anillo" grueso (de un radio al borde de la pantalla).
    if (g_spyglass > 0.001f) {
        const ImVec2 c(d.x * 0.5f, d.y * 0.5f);
        const float radius = d.y * 0.42f;
        const float outer = std::sqrt(d.x * d.x + d.y * d.y) * 0.5f + 4.0f;
        const float thickness = outer - radius;
        const int alpha = static_cast<int>(240.0f * g_spyglass);
        draw->AddCircle(c, radius + thickness * 0.5f, IM_COL32(0, 0, 0, alpha), 96, thickness);
        // Un borde suave hacia adentro (como la lente del catalejo del juego).
        for (int i = 0; i < 6; ++i) {
            const float r = radius - static_cast<float>(i) * 3.0f;
            draw->AddCircle(c, r, IM_COL32(0, 0, 0, static_cast<int>(alpha * 0.18f * (6 - i) / 6.0f)), 96, 3.0f);
        }
    }
    // Barras de cine.
    if (g_bars > 0.0005f) {
        const float h = std::floor(d.y * g_bars);
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(d.x, h), IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled(ImVec2(0, d.y - h), d, IM_COL32(0, 0, 0, 255));
    }
}

}  // namespace luma::render::cinematic
