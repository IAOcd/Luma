// =============================================================================
//  player_hud.cpp - Coordenadas, brújula y velocidad
// =============================================================================
#include "modules/hud/player_hud.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "perf/timer.h"
#include "render/hud.h"
#include "sdk/network.h"
#include "sdk/player.h"

namespace luma::modules::hud {

namespace rh = render::hud;

namespace {

// Los tres módulos comparten UNA lectura del jugador por frame.
bool PlayerThisFrame(sdk::player::Snapshot& out) {
    static int lastFrame = -1;
    static bool lastOk = false;
    static sdk::player::Snapshot cached;
    const int frame = ImGui::GetFrameCount();
    if (frame != lastFrame) {
        lastFrame = frame;
        lastOk = sdk::player::Read(cached);
    }
    out = cached;
    return lastOk;
}

// El rumbo de brújula (0 = norte, 90 = este...) a partir del "yaw" del juego
// (0 = sur, 90 = oeste, 180 = norte, -90 = este).
float Heading(float yaw) {
    float h = std::fmod(yaw + 180.0f, 360.0f);
    return h < 0.0f ? h + 360.0f : h;
}

const char* FacingName(float heading) {
    static const char* const kNames[] = {"Norte (-Z)", "Este (+X)", "Sur (+Z)", "Oeste (-X)"};
    return kNames[static_cast<int>(std::floor((heading + 45.0f) / 90.0f)) % 4];
}

HudLayout ColumnRight(float y) {
    HudLayout l;
    l.x = 0.996f;
    l.y = y;
    l.anchorX = 1.0f;
    return l;
}

// Una caja con varias líneas de texto alineadas a la izquierda.
ImVec2 LinesBox(ImDrawList* draw, const rh::Metrics& m, ImVec2 pos, const char* const* lines, int count) {
    const float u = m.px;
    const float pad = 5.0f * u;
    const float line = m.capHeight + 4.0f * u;
    float width = 58.0f * u;
    for (int i = 0; i < count; ++i) {
        width = std::max(width, rh::TextSize(m, lines[i]).x + pad * 2.0f);
    }
    width = std::floor(width);
    const float height = std::floor(pad * 2.0f + line * static_cast<float>(count) - 4.0f * u);
    rh::Box(draw, m, pos, ImVec2(pos.x + width, pos.y + height));
    for (int i = 0; i < count; ++i) {
        rh::Text(draw, m, ImVec2(pos.x + pad, pos.y + pad + line * static_cast<float>(i) - m.capTop), lines[i],
                 rh::TextColor());
    }
    return ImVec2(width, height);
}

}  // namespace

// =============================================================================
//  Coordenadas
// =============================================================================

Coordinates::Coordinates()
    : Module("coordinates", "Coordenadas", "Muestra tu posición (X, Y, Z) y hacia dónde miras.", Category::Hud) {
    SetDefaultLayout(ColumnRight(0.14f));
}

const char* Coordinates::IconSvg() const {
    return "M12 21C12 21 5 14.5 5 9.5C5 5.91 8.13 3 12 3C15.87 3 19 5.91 19 9.5C19 14.5 12 21 12 21Z "
           "M14.5 9.5C14.5 10.88 13.38 12 12 12C10.62 12 9.5 10.88 9.5 9.5C9.5 8.12 10.62 7 12 7C13.38 7 14.5 8.12 14.5 9.5Z";
}

ImVec2 Coordinates::OnRenderHud(const HudFrame& frame) {
    sdk::player::Snapshot p;
    if (!PlayerThisFrame(p)) {
        if (!frame.editing) {
            return ImVec2(0, 0);
        }
        p.feet[0] = 123.0f;  // ejemplo para el editor de HUD
        p.feet[1] = 64.0f;
        p.feet[2] = -45.0f;
    }
    // Las coordenadas de bloque son el número entero de abajo (floor), igual
    // que las que muestra el juego.
    char lines[5][48];
    int count = 0;
    auto fmt = [&](float v) { return decimals_ ? static_cast<double>(v) : std::floor(static_cast<double>(v)); };
    const char* f = decimals_ ? "%.1f" : "%.0f";
    char x[16], y[16], z[16];
    std::snprintf(x, sizeof(x), f, fmt(p.feet[0]));
    std::snprintf(y, sizeof(y), f, fmt(p.feet[1]));
    std::snprintf(z, sizeof(z), f, fmt(p.feet[2]));
    if (oneLine_) {
        std::snprintf(lines[count++], 48, "%s, %s, %s", x, y, z);
    } else {
        std::snprintf(lines[count++], 48, "X: %s", x);
        std::snprintf(lines[count++], 48, "Y: %s", y);
        std::snprintf(lines[count++], 48, "Z: %s", z);
    }
    if (showNether_) {
        std::snprintf(lines[count++], 48, "Nether: %.0f, %.0f", std::floor(p.feet[0] / 8.0f), std::floor(p.feet[2] / 8.0f));
    }
    if (showFacing_) {
        std::snprintf(lines[count++], 48, "%s", FacingName(Heading(p.yaw)));
    }
    const char* ptrs[5] = {lines[0], lines[1], lines[2], lines[3], lines[4]};
    return LinesBox(frame.draw, rh::Measure(frame.scale), frame.pos, ptrs, count);
}

bool Coordinates::DrawSettings() {
    bool changed = false;
    changed |= ImGui::Checkbox("En una sola línea", &oneLine_);
    changed |= ImGui::Checkbox("Con decimales", &decimals_);
    changed |= ImGui::Checkbox("Hacia dónde miras", &showFacing_);
    changed |= ImGui::Checkbox("Coordenadas del Nether (/8)", &showNether_);
    return changed;
}

void Coordinates::SaveSettings(nlohmann::json& out) const {
    out["oneLine"] = oneLine_;
    out["decimals"] = decimals_;
    out["showFacing"] = showFacing_;
    out["showNether"] = showNether_;
}

void Coordinates::LoadSettings(const nlohmann::json& in) {
    oneLine_ = in.value("oneLine", false);
    decimals_ = in.value("decimals", false);
    showFacing_ = in.value("showFacing", true);
    showNether_ = in.value("showNether", false);
}

// =============================================================================
//  Brújula
// =============================================================================

Compass::Compass()
    : Module("compass", "Brújula", "Una brújula arriba de la pantalla con los puntos cardinales.", Category::Hud) {
    HudLayout l;
    l.x = 0.5f;
    l.y = 0.01f;
    l.anchorX = 0.5f;
    SetDefaultLayout(l);
}

const char* Compass::IconSvg() const {
    return "M21 12C21 16.97 16.97 21 12 21C7.03 21 3 16.97 3 12C3 7.03 7.03 3 12 3C16.97 3 21 7.03 21 12Z "
           "M15.5 8.5L13.5 13.5L8.5 15.5L10.5 10.5Z";
}

ImVec2 Compass::OnRenderHud(const HudFrame& frame) {
    sdk::player::Snapshot p;
    if (!PlayerThisFrame(p)) {
        if (!frame.editing) {
            return ImVec2(0, 0);
        }
        p.yaw = 180.0f;  // ejemplo: mirando al norte
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float width = std::floor(140.0f * u);
    const float height = std::floor(rh::LineBoxHeight(m) + (showDegrees_ ? m.capHeight + 4.0f * u : 0.0f));
    const ImVec2 o = frame.pos;
    ImDrawList* draw = frame.draw;
    rh::Box(draw, m, o, ImVec2(o.x + width, o.y + height));

    // La tira de la brújula: 120° visibles. Cada punto cardinal se dibuja a
    // la distancia (en grados) que está de hacia donde miras.
    const float heading = Heading(p.yaw);
    constexpr float kVisible = 120.0f;
    const float centerX = o.x + width * 0.5f;
    const float bandTop = o.y;
    const float bandH = rh::LineBoxHeight(m);
    static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
    draw->PushClipRect(o, ImVec2(o.x + width, o.y + height), true);
    for (int i = 0; i < 8; ++i) {
        float delta = static_cast<float>(i) * 45.0f - heading;
        delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;  // a -180..180
        if (std::fabs(delta) > kVisible * 0.5f + 10.0f) {
            continue;
        }
        const float x = centerX + delta / kVisible * width;
        const bool main = i % 2 == 0;
        const ImVec2 size = rh::TextSize(m, kPoints[i]);
        // Más tenue cuanto más lejos del centro.
        const float fade = 1.0f - std::min(1.0f, std::fabs(delta) / (kVisible * 0.5f)) * 0.7f;
        rh::Text(draw, m, ImVec2(x - size.x * 0.5f, bandTop + (bandH - m.capHeight) * 0.5f - m.capTop), kPoints[i],
                 rh::TextColor((main ? 1.0f : 0.6f) * fade));
    }
    // Marcas cada 15°.
    for (int d = 0; d < 360; d += 15) {
        float delta = static_cast<float>(d) - heading;
        delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
        if (std::fabs(delta) > kVisible * 0.5f || d % 45 == 0) {
            continue;
        }
        const float x = std::floor(centerX + delta / kVisible * width);
        draw->AddRectFilled(ImVec2(x, bandTop + bandH - 4.0f * u), ImVec2(x + std::max(1.0f, u * 0.5f), bandTop + bandH - 2.0f * u),
                            rh::TextColor(0.4f));
    }
    draw->PopClipRect();
    // Marca del centro (hacia donde miras).
    draw->AddRectFilled(ImVec2(std::floor(centerX - u * 0.5f), o.y), ImVec2(std::floor(centerX + u * 0.5f) + 1.0f, o.y + 2.0f * u),
                        rh::TextColor());
    if (showDegrees_) {
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f°", heading);
        rh::CenteredText(draw, m, ImVec2(o.x, o.y + bandH - 2.0f * u), ImVec2(o.x + width, o.y + height), text,
                         rh::TextColor(0.8f));
    }
    return ImVec2(width, height);
}

bool Compass::DrawSettings() {
    return ImGui::Checkbox("Mostrar grados", &showDegrees_);
}

void Compass::SaveSettings(nlohmann::json& out) const {
    out["showDegrees"] = showDegrees_;
}

void Compass::LoadSettings(const nlohmann::json& in) {
    showDegrees_ = in.value("showDegrees", true);
}

// =============================================================================
//  Velocidad
// =============================================================================

SpeedDisplay::SpeedDisplay()
    : Module("speed", "Velocidad", "Tu velocidad en bloques por segundo.", Category::Hud) {
    SetDefaultLayout(ColumnRight(0.30f));
}

const char* SpeedDisplay::IconSvg() const {
    return "M4 17C4 12.58 7.58 9 12 9C16.42 9 20 12.58 20 17 M12 17L17 11 M4 17h2 M18 17h2";
}

ImVec2 SpeedDisplay::OnRenderHud(const HudFrame& frame) {
    sdk::player::Snapshot p;
    const bool ok = PlayerThisFrame(p);
    if (!ok && !frame.editing) {
        lastSample_ = 0;
        return ImVec2(0, 0);
    }
    if (ok) {
        // Distancia recorrida desde la última muestra, dividida por el tiempo.
        // Muestras cada 100 ms y un suavizado leve para que no tiemble.
        const std::int64_t now = perf::Now();
        const double elapsed = lastSample_ == 0 ? 0.0 : perf::TicksToMs(now - lastSample_) / 1000.0;
        if (lastSample_ == 0 || elapsed >= 0.1) {
            if (lastSample_ != 0 && elapsed < 1.0) {
                const float dx = p.feet[0] - lastPos_[0];
                const float dy = vertical_ ? p.feet[1] - lastPos_[1] : 0.0f;
                const float dz = p.feet[2] - lastPos_[2];
                const float speed = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / elapsed);
                shown_ = speed > 200.0f ? 0.0f : shown_ + (speed - shown_) * 0.6f;  // >200: teletransporte
            }
            lastSample_ = now;
            std::copy(p.feet, p.feet + 3, lastPos_);
        }
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f m/s", ok ? shown_ : 4.32f);
    return rh::TextBox(frame.draw, rh::Measure(frame.scale), frame.pos, text, 58.0f);
}

bool SpeedDisplay::DrawSettings() {
    return ImGui::Checkbox("Incluir la velocidad vertical", &vertical_);
}

void SpeedDisplay::SaveSettings(nlohmann::json& out) const {
    out["vertical"] = vertical_;
}

void SpeedDisplay::LoadSettings(const nlohmann::json& in) {
    vertical_ = in.value("vertical", false);
}

// =============================================================================
//  Ping
// =============================================================================

PingDisplay::PingDisplay()
    : Module("ping", "Ping", "Tu ping al servidor (ms). En un mundo local no hay ping.", Category::Hud) {
    SetDefaultLayout(ColumnRight(0.38f));
}

const char* PingDisplay::IconSvg() const {
    return "M4 20h2v-3H4z M9 20h2v-6H9z M14 20h2v-9h-2z M19 20h2V6h-2z";
}

ImVec2 PingDisplay::OnRenderHud(const HudFrame& frame) {
    const int ping = sdk::network::PingMs();
    char text[32];
    if (ping >= 0) {
        std::snprintf(text, sizeof(text), "%d ms", ping);
    } else if (showInLocal_ || frame.editing) {
        std::snprintf(text, sizeof(text), "%s", frame.editing && !showInLocal_ ? "42 ms" : "Mundo local");
    } else {
        return ImVec2(0, 0);
    }
    return rh::TextBox(frame.draw, rh::Measure(frame.scale), frame.pos, text, 58.0f);
}

bool PingDisplay::DrawSettings() {
    return ImGui::Checkbox("Mostrar \"Mundo local\" sin servidor", &showInLocal_);
}

void PingDisplay::SaveSettings(nlohmann::json& out) const {
    out["showInLocal"] = showInLocal_;
}

void PingDisplay::LoadSettings(const nlohmann::json& in) {
    showInLocal_ = in.value("showInLocal", false);
}

}  // namespace luma::modules::hud
