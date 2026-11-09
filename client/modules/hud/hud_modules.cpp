// =============================================================================
//  hud_modules.cpp - FPS, CPS, Keystrokes y Reloj
// =============================================================================
#include "modules/hud/hud_modules.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "core/clicks.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/hud.h"
#include "render/input.h"

namespace luma::modules::hud {

namespace rh = render::hud;  // el estilo común del HUD (render/hud.h)

namespace {

// Ancho mínimo de las cajas de texto (en píxeles de HUD): igual al ancho de
// los keystrokes, así quedan todas parejas en una columna.
constexpr float kBoxWidth = 58.0f;

// Posiciones de fábrica: una columna arriba a la izquierda (fracciones de la
// pantalla, pensadas para 1080p).
HudLayout Column(float y) {
    HudLayout l;
    l.x = 0.004f;
    l.y = y;
    return l;
}

// ¿La ventana del juego está en primer plano? (si no, no mostramos teclas
// apretadas: las estarías usando en otro programa).
bool GameFocused() {
    const HWND game = render::input::GameWindow();
    return game != nullptr && GetForegroundWindow() == game;
}

bool KeyDown(int vk) {
    // Luma llama a la GetAsyncKeyState real (el bloqueo del menú no la afecta).
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Mezcla dos colores ImU32 (t = 0 -> a, 1 -> b).
ImU32 MixColor(ImU32 a, ImU32 b, float t) {
    const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                                                 ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

}  // namespace

// =============================================================================
//  FPS
// =============================================================================

FpsCounter::FpsCounter()
    : Module("fps", "FPS", "Muestra los cuadros por segundo del juego.", Category::Hud) {
    SetDefaultLayout(Column(0.130f));
}

const char* FpsCounter::IconSvg() const {
    return "M4 17C4 12.58 7.58 9 12 9C16.42 9 20 12.58 20 17 M12 17L15.5 12.5 M4 17h2 M18 17h2";
}

ImVec2 FpsCounter::OnRenderHud(const HudFrame& frame) {
    // Actualizar el número 4 veces por segundo (si no, cambia cada frame y
    // no se puede leer).
    const std::int64_t now = perf::Now();
    if (lastUpdate_ == 0 || perf::TicksToMs(now - lastUpdate_) >= 250.0) {
        lastUpdate_ = now;
        const perf::Recent recent = perf::RecentAverage(60);
        shown_ = recent.frameMs > 0.0 ? 1000.0 / recent.frameMs : 0.0;
    }
    char text[32];
    if (format_ == 1) {
        std::snprintf(text, sizeof(text), "FPS: %.0f", shown_);
    } else {
        std::snprintf(text, sizeof(text), "%.0f FPS", shown_);
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    return rh::TextBox(frame.draw, m, frame.pos, text, kBoxWidth);
}

bool FpsCounter::DrawSettings() {
    const char* formats[] = {"60 FPS", "FPS: 60"};
    return ImGui::Combo("Formato", &format_, formats, IM_ARRAYSIZE(formats));
}

void FpsCounter::SaveSettings(nlohmann::json& out) const {
    out["format"] = format_;
}

void FpsCounter::LoadSettings(const nlohmann::json& in) {
    format_ = std::clamp(in.value("format", 0), 0, 1);
}

// =============================================================================
//  CPS
// =============================================================================

CpsCounter::CpsCounter()
    : Module("cps", "CPS", "Clics por segundo del mouse (izquierdo y derecho).", Category::Hud) {
    SetDefaultLayout(Column(0.166f));
}

const char* CpsCounter::IconSvg() const {
    return "M12 3v5 M7 4.5l2 3.5 M17 4.5l-2 3.5 M8 11h8v6C16 19.2 14.2 21 12 21C9.8 21 8 19.2 8 17Z M12 11v4";
}

void CpsCounter::OnEnable() {
    clicks::Acquire();
}

void CpsCounter::OnDisable() {
    clicks::Release();
}

ImVec2 CpsCounter::OnRenderHud(const HudFrame& frame) {
    char text[32];
    const int left = clicks::PerSecond(clicks::Button::Left);
    if (showRight_) {
        std::snprintf(text, sizeof(text), "%d | %d CPS", left, clicks::PerSecond(clicks::Button::Right));
    } else {
        std::snprintf(text, sizeof(text), "%d CPS", left);
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    return rh::TextBox(frame.draw, m, frame.pos, text, kBoxWidth);
}

bool CpsCounter::DrawSettings() {
    return ImGui::Checkbox("Mostrar también el clic derecho", &showRight_);
}

void CpsCounter::SaveSettings(nlohmann::json& out) const {
    out["showRight"] = showRight_;
}

void CpsCounter::LoadSettings(const nlohmann::json& in) {
    showRight_ = in.value("showRight", true);
}

// =============================================================================
//  Keystrokes
// =============================================================================

Keystrokes::Keystrokes()
    : Module("keystrokes", "Keystrokes", "Muestra en pantalla las teclas WASD, el espacio y los clics que apretas.",
             Category::Hud) {
    SetDefaultLayout(Column(0.007f));
}

const char* Keystrokes::IconSvg() const {
    return "M9.5 3h5v5h-5z M3.5 10h5v5h-5z M9.5 10h5v5h-5z M15.5 10h5v5h-5z M3.5 17h17v4h-17z";
}

void Keystrokes::OnEnable() {
    clicks::Acquire();
}

void Keystrokes::OnDisable() {
    clicks::Release();
}

ImVec2 Keystrokes::OnRenderHud(const HudFrame& frame) {
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;            // un píxel de HUD
    const float key = 18.0f * u;     // lado de una tecla
    const float gap = 2.0f * u;      // separación
    const float width = key * 3.0f + gap * 2.0f;
    const ImVec2 o = frame.pos;
    ImDrawList* draw = frame.draw;

    // Estado de cada tecla y animación suave (se "enciende" en ~50 ms).
    const bool focused = GameFocused();
    const bool down[7] = {focused && KeyDown('W'),       focused && KeyDown('A'),
                          focused && KeyDown('S'),       focused && KeyDown('D'),
                          focused && KeyDown(VK_SPACE),  focused && KeyDown(VK_LBUTTON),
                          focused && KeyDown(VK_RBUTTON)};
    const float step = std::min(1.0f, ImGui::GetIO().DeltaTime * 20.0f);
    for (std::size_t i = 0; i < anim_.size(); ++i) {
        anim_[i] += ((down[i] ? 1.0f : 0.0f) - anim_[i]) * step;
    }

    const ImU32 text = rh::TextColor();
    const ImU32 dark = IM_COL32(20, 22, 24, 255);  // texto sobre una tecla apretada
    auto keyBox = [&](ImVec2 min, ImVec2 size, const char* label, float a) {
        const ImVec2 max(min.x + size.x, min.y + size.y);
        rh::Box(draw, m, min, max, a);
        rh::CenteredText(draw, m, min, max, label, MixColor(text, dark, a));
    };

    // Fila 1: W, centrada. Fila 2: A S D.
    keyBox(ImVec2(o.x + key + gap, o.y), ImVec2(key, key), "W", anim_[0]);
    const float row2 = o.y + key + gap;
    keyBox(ImVec2(o.x, row2), ImVec2(key, key), "A", anim_[1]);
    keyBox(ImVec2(o.x + key + gap, row2), ImVec2(key, key), "S", anim_[2]);
    keyBox(ImVec2(o.x + (key + gap) * 2.0f, row2), ImVec2(key, key), "D", anim_[3]);
    float y = row2 + key + gap;

    // Fila 3: botones del mouse. Si estás haciendo clic, muestran los CPS.
    if (showMouse_) {
        const float half = (width - gap) * 0.5f;
        const float height = 15.0f * u;
        char left[16] = "LMB";
        char right[16] = "RMB";
        if (showCps_) {
            const int l = clicks::PerSecond(clicks::Button::Left);
            const int r = clicks::PerSecond(clicks::Button::Right);
            if (l > 0) std::snprintf(left, sizeof(left), "%d CPS", l);
            if (r > 0) std::snprintf(right, sizeof(right), "%d CPS", r);
        }
        keyBox(ImVec2(o.x, y), ImVec2(half, height), left, anim_[5]);
        keyBox(ImVec2(o.x + half + gap, y), ImVec2(half, height), right, anim_[6]);
        y += height + gap;
    }
    // Fila 4: la barra espaciadora (una caja baja con una rayita).
    if (showSpace_) {
        const float height = 10.0f * u;
        const ImVec2 min(o.x, y);
        const ImVec2 max(o.x + width, y + height);
        rh::Box(draw, m, min, max, anim_[4]);
        const float barW = width * 0.45f;
        const float barY = std::floor(min.y + height * 0.5f - u * 0.5f);
        const ImVec2 b0(std::floor(min.x + (width - barW) * 0.5f), barY);
        draw->AddRectFilled(b0, ImVec2(b0.x + std::floor(barW), barY + std::max(1.0f, u)),
                            MixColor(text, dark, anim_[4]));
        y += height + gap;
    }
    return ImVec2(width, y - gap - o.y);
}

bool Keystrokes::DrawSettings() {
    bool changed = false;
    changed |= ImGui::Checkbox("Botones del mouse", &showMouse_);
    changed |= ImGui::Checkbox("Barra espaciadora", &showSpace_);
    ImGui::BeginDisabled(!showMouse_);
    changed |= ImGui::Checkbox("Mostrar CPS al hacer clic", &showCps_);
    ImGui::EndDisabled();
    return changed;
}

void Keystrokes::SaveSettings(nlohmann::json& out) const {
    out["showMouse"] = showMouse_;
    out["showSpace"] = showSpace_;
    out["showCps"] = showCps_;
}

void Keystrokes::LoadSettings(const nlohmann::json& in) {
    showMouse_ = in.value("showMouse", true);
    showSpace_ = in.value("showSpace", true);
    showCps_ = in.value("showCps", true);
}

// =============================================================================
//  Reloj
// =============================================================================

Clock::Clock() : Module("clock", "Reloj", "Muestra la hora de tu PC.", Category::Hud) {
    SetDefaultLayout(Column(0.202f));
}

const char* Clock::IconSvg() const {
    return "M20 12C20 16.42 16.42 20 12 20C7.58 20 4 16.42 4 12C4 7.58 7.58 4 12 4C16.42 4 20 7.58 20 12Z M12 7v5l3 2";
}

ImVec2 Clock::OnRenderHud(const HudFrame& frame) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[32];
    if (twelveHour_) {
        const int hour = t.wHour % 12 == 0 ? 12 : t.wHour % 12;
        const char* suffix = t.wHour < 12 ? "AM" : "PM";
        if (seconds_) {
            std::snprintf(text, sizeof(text), "%d:%02d:%02d %s", hour, t.wMinute, t.wSecond, suffix);
        } else {
            std::snprintf(text, sizeof(text), "%d:%02d %s", hour, t.wMinute, suffix);
        }
    } else if (seconds_) {
        std::snprintf(text, sizeof(text), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    } else {
        std::snprintf(text, sizeof(text), "%02d:%02d", t.wHour, t.wMinute);
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    return rh::TextBox(frame.draw, m, frame.pos, text, kBoxWidth);
}

bool Clock::DrawSettings() {
    bool changed = false;
    changed |= ImGui::Checkbox("Formato de 12 horas (AM/PM)", &twelveHour_);
    changed |= ImGui::Checkbox("Mostrar segundos", &seconds_);
    return changed;
}

void Clock::SaveSettings(nlohmann::json& out) const {
    out["twelveHour"] = twelveHour_;
    out["seconds"] = seconds_;
}

void Clock::LoadSettings(const nlohmann::json& in) {
    twelveHour_ = in.value("twelveHour", false);
    seconds_ = in.value("seconds", false);
}

}  // namespace luma::modules::hud
