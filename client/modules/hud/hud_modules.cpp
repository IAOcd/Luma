// =============================================================================
//  hud_modules.cpp - FPS, CPS, Keystrokes y Reloj
// =============================================================================
#include "modules/hud/hud_modules.h"
#include "core/tr.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/clicks.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/hud.h"
#include "render/input.h"
#include "render/ui.h"

namespace luma::modules::hud {

namespace rh = render::hud;  // el estilo común del HUD (render/hud.h)
namespace ui = render::ui;   // controles para las opciones (render/ui.h)

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

// Una caja con texto en un color dado.
ImVec2 ColoredTextBox(const HudFrame& frame, const char* text, ImU32 color) {
    const rh::Metrics m = rh::Measure(frame.scale);
    const ImVec2 textSize = rh::TextSize(m, text);
    const float width = std::floor(std::max(textSize.x + 10.0f * m.px, kBoxWidth * m.px));
    const float height = std::floor(rh::LineBoxHeight(m));
    const ImVec2 max(frame.pos.x + width, frame.pos.y + height);
    rh::Box(frame.draw, m, frame.pos, max);
    rh::CenteredText(frame.draw, m, frame.pos, max, text, color);
    return ImVec2(width, height);
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
    // Actualizar el número unas veces por segundo (si no, cambia cada frame
    // y no se puede leer).
    static constexpr double kRates[] = {100.0, 250.0, 1000.0};
    const std::int64_t now = perf::Now();
    if (lastUpdate_ == 0 || perf::TicksToMs(now - lastUpdate_) >= kRates[std::clamp(rate_, 0, 2)]) {
        lastUpdate_ = now;
        const perf::Recent recent = perf::RecentAverage(60);
        shown_ = recent.frameMs > 0.0 ? 1000.0 / recent.frameMs : 0.0;
        shownMs_ = recent.frameMs;
    }
    char number[24];
    std::snprintf(number, sizeof(number), showMs_ ? "%.0f (%.1f ms)" : "%.0f", shown_, shownMs_);
    char text[48];
    if (format_ == 1) {
        std::snprintf(text, sizeof(text), "FPS: %s", number);
    } else if (format_ == 2) {
        std::snprintf(text, sizeof(text), "%s", number);
    } else {
        std::snprintf(text, sizeof(text), "%s FPS", number);
    }
    ImU32 color = rh::TextColor();
    if (colorize_) {
        color = shown_ >= 60.0 ? IM_COL32(110, 230, 110, 255) : shown_ >= 30.0 ? IM_COL32(240, 210, 80, 255)
                                                                              : IM_COL32(240, 90, 80, 255);
    }
    return ColoredTextBox(frame, text, color);
}

bool FpsCounter::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Formato", &format_, {"60 FPS", "FPS: 60", "60"});
    changed |= ui::Toggle("Mostrar el frametime", &showMs_, "(ms por cuadro)");
    changed |= ui::Toggle("Color según los FPS", &colorize_, "(verde, amarillo, rojo)");
    changed |= ui::Choice("Actualizar", &rate_, {"Rápido", "Normal", "Lento"});
    return changed;
}

void FpsCounter::SaveSettings(nlohmann::json& out) const {
    out["format"] = format_;
    out["rate"] = rate_;
    out["showMs"] = showMs_;
    out["colorize"] = colorize_;
}

void FpsCounter::LoadSettings(const nlohmann::json& in) {
    format_ = std::clamp(in.value("format", 0), 0, 2);
    rate_ = std::clamp(in.value("rate", 1), 0, 2);
    showMs_ = in.value("showMs", false);
    colorize_ = in.value("colorize", false);
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
    char number[24];
    const int left = clicks::PerSecond(clicks::Button::Left);
    if (showRight_) {
        std::snprintf(number, sizeof(number), "%d | %d", left, clicks::PerSecond(clicks::Button::Right));
    } else {
        std::snprintf(number, sizeof(number), "%d", left);
    }
    char text[32];
    std::snprintf(text, sizeof(text), format_ == 1 ? "CPS: %s" : "%s CPS", number);
    return ColoredTextBox(frame, text, rh::TextColor());
}

bool CpsCounter::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Formato", &format_, {"3 CPS", "CPS: 3"});
    changed |= ui::Toggle("También el clic derecho", &showRight_, "(izq. | der.)");
    return changed;
}

void CpsCounter::SaveSettings(nlohmann::json& out) const {
    out["showRight"] = showRight_;
    out["format"] = format_;
}

void CpsCounter::LoadSettings(const nlohmann::json& in) {
    showRight_ = in.value("showRight", true);
    format_ = std::clamp(in.value("format", 0), 0, 1);
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
    const float u = m.px;                                // un píxel de HUD
    const float key = static_cast<float>(keySize_) * u;  // lado de una tecla
    const float gap = static_cast<float>(gap_) * u;      // separación
    const float width = key * 3.0f + gap * 2.0f;
    const ImVec2 o = frame.pos;
    ImDrawList* draw = frame.draw;

    // Estado de cada tecla y animación (se "enciende" según la velocidad elegida).
    const bool focused = GameFocused();
    const bool down[7] = {focused && KeyDown('W'),       focused && KeyDown('A'),
                          focused && KeyDown('S'),       focused && KeyDown('D'),
                          focused && KeyDown(VK_SPACE),  focused && KeyDown(VK_LBUTTON),
                          focused && KeyDown(VK_RBUTTON)};
    static constexpr float kSpeeds[] = {1000.0f, 20.0f, 9.0f};
    const float step = std::min(1.0f, ImGui::GetIO().DeltaTime * kSpeeds[std::clamp(animation_, 0, 2)]);
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
        const float half = std::floor((width - gap) * 0.5f);
        const float height = std::floor(key * 0.83f);
        char left[16];
        char right[16];
        std::snprintf(left, sizeof(left), "%s", mouseLabels_ == 1 ? Tr("Izq.") : "LMB");
        std::snprintf(right, sizeof(right), "%s", mouseLabels_ == 1 ? Tr("Der.") : "RMB");
        if (showCps_) {
            const int l = clicks::PerSecond(clicks::Button::Left);
            const int r = clicks::PerSecond(clicks::Button::Right);
            if (l > 0) std::snprintf(left, sizeof(left), "%d CPS", l);
            if (r > 0) std::snprintf(right, sizeof(right), "%d CPS", r);
        }
        keyBox(ImVec2(o.x, y), ImVec2(half, height), left, anim_[5]);
        keyBox(ImVec2(o.x + width - half, y), ImVec2(half, height), right, anim_[6]);
        y += height + gap;
    }
    // Fila 4: la barra espaciadora (una caja baja con una rayita).
    if (showSpace_) {
        const float height = std::floor(key * 0.55f);
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
    ui::Section("Qué mostrar");
    changed |= ui::Toggle("Botones del mouse", &showMouse_);
    changed |= ui::Toggle("Barra espaciadora", &showSpace_);
    changed |= ui::Toggle("CPS al hacer clic", &showCps_, "(en los botones)");
    changed |= ui::Choice("Botones", &mouseLabels_, {"LMB / RMB", "Izq. / Der."});
    ui::Section("Forma");
    changed |= ui::SliderInt("Tamaño de las teclas", &keySize_, 12, 26, "%d");
    changed |= ui::SliderInt("Separación", &gap_, 0, 6, "%d");
    changed |= ui::Choice("Animación", &animation_, {"Instantánea", "Rápida", "Suave"});
    ui::Hint("El color de una tecla apretada se cambia en Estilo (\"Color al apretar\").");
    return changed;
}

void Keystrokes::SaveSettings(nlohmann::json& out) const {
    out["showMouse"] = showMouse_;
    out["showSpace"] = showSpace_;
    out["showCps"] = showCps_;
    out["keySize"] = keySize_;
    out["gap"] = gap_;
    out["animation"] = animation_;
    out["mouseLabels"] = mouseLabels_;
}

void Keystrokes::LoadSettings(const nlohmann::json& in) {
    showMouse_ = in.value("showMouse", true);
    showSpace_ = in.value("showSpace", true);
    showCps_ = in.value("showCps", true);
    keySize_ = std::clamp(in.value("keySize", 18), 12, 26);
    gap_ = std::clamp(in.value("gap", 2), 0, 6);
    animation_ = std::clamp(in.value("animation", 1), 0, 2);
    mouseLabels_ = std::clamp(in.value("mouseLabels", 0), 0, 1);
}

// =============================================================================
//  Reloj
// =============================================================================

Clock::Clock() : Module("clock", "Reloj", "Muestra la hora (y si quieres, la fecha) de tu PC.", Category::Hud) {
    SetDefaultLayout(Column(0.202f));
}

const char* Clock::IconSvg() const {
    return "M20 12C20 16.42 16.42 20 12 20C7.58 20 4 16.42 4 12C4 7.58 7.58 4 12 4C16.42 4 20 7.58 20 12Z M12 7v5l3 2";
}

ImVec2 Clock::OnRenderHud(const HudFrame& frame) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char time[24];
    if (format_ == 1) {
        const int hour = t.wHour % 12 == 0 ? 12 : t.wHour % 12;
        const char* suffix = t.wHour < 12 ? "AM" : "PM";
        if (seconds_) {
            std::snprintf(time, sizeof(time), "%d:%02d:%02d %s", hour, t.wMinute, t.wSecond, suffix);
        } else {
            std::snprintf(time, sizeof(time), "%d:%02d %s", hour, t.wMinute, suffix);
        }
    } else if (seconds_) {
        std::snprintf(time, sizeof(time), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    } else {
        std::snprintf(time, sizeof(time), "%02d:%02d", t.wHour, t.wMinute);
    }
    char text[48];
    if (date_ == 1) {
        std::snprintf(text, sizeof(text), "%02d/%02d  %s", t.wDay, t.wMonth, time);
    } else if (date_ == 2) {
        std::snprintf(text, sizeof(text), "%02d/%02d/%04d  %s", t.wDay, t.wMonth, t.wYear, time);
    } else {
        std::snprintf(text, sizeof(text), "%s", time);
    }
    return ColoredTextBox(frame, text, rh::TextColor());
}

bool Clock::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Formato", &format_, {"24 horas", "12 horas"});
    changed |= ui::Toggle("Segundos", &seconds_);
    changed |= ui::Choice("Fecha", &date_, {"Sin fecha", "09/11", "09/11/2026"});
    return changed;
}

void Clock::SaveSettings(nlohmann::json& out) const {
    out["format"] = format_;
    out["seconds"] = seconds_;
    out["date"] = date_;
}

void Clock::LoadSettings(const nlohmann::json& in) {
    // "twelveHour" era el nombre anterior de la opción (perfiles viejos).
    format_ = std::clamp(in.value("format", in.value("twelveHour", false) ? 1 : 0), 0, 1);
    seconds_ = in.value("seconds", false);
    date_ = std::clamp(in.value("date", 0), 0, 2);
}

}  // namespace luma::modules::hud
