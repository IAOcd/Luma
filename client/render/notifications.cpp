// =============================================================================
//  notifications.cpp - Implementación de los toasts
// =============================================================================
//  Animación: cada aviso entra deslizándose desde la derecha y aparece
//  gradualmente (0,25 s), se queda, y se desvanece (0,4 s). Es solo calcular
//  una posición y una transparencia por aviso: prácticamente gratis.
// =============================================================================
#include "render/notifications.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

#include "core/logger.h"
#include "perf/timer.h"
#include "render/glass.h"
#include "render/icons.h"
#include "render/theme.h"

namespace luma::notify {

namespace {

constexpr std::size_t kMaxToasts = 6;
constexpr double kLifetimeSeconds = 3.5;
constexpr double kFadeInSeconds = 0.25;
constexpr double kFadeOutSeconds = 0.4;

struct Toast {
    Kind kind = Kind::Info;
    std::string text;
    std::int64_t createdAt = 0;  // ticks del reloj de alta precisión
};

std::mutex g_mutex;
std::array<Toast, kMaxToasts> g_toasts;
std::size_t g_count = 0;  // los primeros g_count elementos son válidos (el 0 es el más viejo)
std::atomic<bool> g_hasActive{false};

double SecondsSince(std::int64_t ticks) {
    return perf::TicksToMs(perf::Now() - ticks) / 1000.0;
}

ImVec4 KindColor(Kind kind) {
    switch (kind) {
        case Kind::Success: return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
        case Kind::Warning: return ImVec4(1.00f, 0.75f, 0.25f, 1.0f);
        case Kind::Error:   return ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
        case Kind::Info:    break;
    }
    return theme::Accent(1.0f);
}

// Curva de animación "ease out": empieza rápido y frena al final. Se ve más
// natural que un movimiento a velocidad constante.
float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

}  // namespace

void Push(Kind kind, std::string text) {
    std::lock_guard lock(g_mutex);
    if (g_count == kMaxToasts) {
        // Lleno: descartamos el más viejo corriendo todos un lugar.
        std::move(g_toasts.begin() + 1, g_toasts.end(), g_toasts.begin());
        --g_count;
    }
    g_toasts[g_count] = Toast{kind, std::move(text), perf::Now()};
    ++g_count;
    g_hasActive.store(true);
}

bool HasActive() {
    return g_hasActive.load(std::memory_order_relaxed);
}

void Render() {
    std::lock_guard lock(g_mutex);

    // Quitar los vencidos (siempre son los primeros, porque son los más viejos).
    std::size_t expired = 0;
    while (expired < g_count && SecondsSince(g_toasts[expired].createdAt) > kLifetimeSeconds) {
        ++expired;
    }
    if (expired > 0) {
        std::move(g_toasts.begin() + static_cast<std::ptrdiff_t>(expired),
                  g_toasts.begin() + static_cast<std::ptrdiff_t>(g_count), g_toasts.begin());
        g_count -= expired;
    }
    g_hasActive.store(g_count > 0);
    if (g_count == 0) {
        return;
    }

    const bool animate = theme::Get().animations;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float fontSize = ImGui::GetFontSize();
    const float padding = fontSize * 0.6f;
    const float margin = fontSize;
    const float rounding = theme::Get().rounding * theme::UiScale() * 0.6f;
    float y = display.y - margin;

    // Del más nuevo (abajo) al más viejo (arriba).
    for (std::size_t i = g_count; i-- > 0;) {
        const Toast& toast = g_toasts[i];
        const double age = SecondsSince(toast.createdAt);
        float alpha = 1.0f;
        float slide = 0.0f;
        if (animate) {
            const float in = EaseOut(static_cast<float>(age / kFadeInSeconds));
            const float out = static_cast<float>(
                std::clamp((kLifetimeSeconds - age) / kFadeOutSeconds, 0.0, 1.0));
            alpha = std::min(in, out);
            slide = (1.0f - in) * fontSize * 4.0f;
        }

        // Píldora de vidrio con un ícono en un círculo de color según el tipo.
        const ImVec2 textSize = ImGui::CalcTextSize(toast.text.c_str());
        const float height = textSize.y + padding * 1.8f;
        const float circle = height - padding * 0.9f;
        const float width = textSize.x + padding * 2.4f + circle;
        const ImVec2 min(display.x - margin - width + slide, y - height);
        const ImVec2 max(min.x + width, y);
        const float r = std::min(height * 0.5f, rounding * 1.4f);
        render::glass::Panel(draw, min, max, r, alpha);

        ImVec4 color = KindColor(toast.kind);
        color.w = alpha;
        const ImVec2 c(min.x + padding * 0.45f + circle * 0.5f, min.y + height * 0.5f);
        draw->AddCircleFilled(c, circle * 0.5f, ImGui::ColorConvertFloat4ToU32(color), 32);
        const char* icon = toast.kind == Kind::Success   ? render::icons::kCheck
                           : toast.kind == Kind::Info    ? render::icons::kSparkle
                                                         : render::icons::kClose;
        ImVec4 onColor = theme::P().onAccentText;
        if (toast.kind != Kind::Info) {
            onColor = ImVec4(1, 1, 1, 1);
        }
        onColor.w = alpha;
        render::icons::Draw(draw, icon, c, circle * 0.6f, ImGui::ColorConvertFloat4ToU32(onColor));
        ImVec4 text = theme::P().text;
        text.w *= alpha;
        draw->AddText(ImVec2(c.x + circle * 0.5f + padding * 0.7f, min.y + (height - textSize.y) * 0.5f),
                      ImGui::ColorConvertFloat4ToU32(text), toast.text.c_str());

        y -= height + fontSize * 0.45f;
    }
}

void Clear() {
    std::lock_guard lock(g_mutex);
    for (std::size_t i = 0; i < g_count; ++i) {
        g_toasts[i] = Toast{};
    }
    g_count = 0;
    g_hasActive.store(false);
}

}  // namespace luma::notify
