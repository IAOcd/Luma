// =============================================================================
//  player_hud.cpp - Coordenadas, brújula y velocidad
// =============================================================================
#include "modules/hud/player_hud.h"
#include "core/tr.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "perf/timer.h"
#include "render/hud.h"
#include "render/ui.h"
#include "sdk/hud_ui.h"
#include "sdk/network.h"
#include "sdk/player.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace ui = render::ui;

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
    static const char* const kNames[] = {"North (-Z)", "East (+X)", "South (+Z)", "West (-X)"};
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
    : Module("coordinates", "Coordinates", "Shows your position (X, Y, Z) and where you're facing.", Category::Hud) {
    SetDefaultLayout(ColumnRight(0.14f));
}

const char* Coordinates::IconSvg() const {
    return "M12 21C12 21 5 14.5 5 9.5C5 5.91 8.13 3 12 3C15.87 3 19 5.91 19 9.5C19 14.5 12 21 12 21Z "
           "M14.5 9.5C14.5 10.88 13.38 12 12 12C10.62 12 9.5 10.88 9.5 9.5C9.5 8.12 10.62 7 12 7C13.38 7 14.5 8.12 14.5 9.5Z";
}

void Coordinates::OnEnable() {
    sdk::hudui::SetHideCoords(hideVanilla_);
}

void Coordinates::OnDisable() {
    sdk::hudui::SetHideCoords(false);  // las del juego vuelven a verse
}

ImVec2 Coordinates::OnRenderHud(const HudFrame& frame) {
    sdk::hudui::SetHideCoords(hideVanilla_);  // (barato: solo guarda un sí/no)
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
    int layout = oneLine_ ? 1 : 0;
    if (ui::Choice("Layout", &layout, {"Three lines", "One line"})) {
        oneLine_ = layout == 1;
        changed = true;
    }
    changed |= ui::Toggle("Decimals", &decimals_);
    changed |= ui::Toggle("Facing", &showFacing_);
    changed |= ui::Toggle("Nether coordinates", &showNether_, "(÷ 8)");
    if (ui::Toggle("Hide the game's coordinates", &hideVanilla_, "(the \"Show coordinates\" ones)")) {
        sdk::hudui::SetHideCoords(hideVanilla_ && IsEnabled());
        changed = true;
    }
    return changed;
}

void Coordinates::SaveSettings(nlohmann::json& out) const {
    out["oneLine"] = oneLine_;
    out["decimals"] = decimals_;
    out["showFacing"] = showFacing_;
    out["showNether"] = showNether_;
    out["hideVanilla"] = hideVanilla_;
}

void Coordinates::LoadSettings(const nlohmann::json& in) {
    oneLine_ = in.value("oneLine", false);
    decimals_ = in.value("decimals", false);
    showFacing_ = in.value("showFacing", true);
    showNether_ = in.value("showNether", false);
    hideVanilla_ = in.value("hideVanilla", true);
}

// =============================================================================
//  Brújula
// =============================================================================

Compass::Compass()
    : Module("compass", "Compass", "A compass at the top of the screen with the cardinal points.", Category::Hud) {
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
    const float width = std::floor(static_cast<float>(width_) * u);
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
    static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "O", "NW"};
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
    bool changed = false;
    changed |= ui::Toggle("Degrees", &showDegrees_);
    changed |= ui::SliderInt("Width", &width_, 80, 240, "%d");
    return changed;
}

void Compass::SaveSettings(nlohmann::json& out) const {
    out["showDegrees"] = showDegrees_;
    out["width"] = width_;
}

void Compass::LoadSettings(const nlohmann::json& in) {
    showDegrees_ = in.value("showDegrees", true);
    width_ = std::clamp(in.value("width", 140), 80, 240);
}

// =============================================================================
//  Velocidad
// =============================================================================

SpeedDisplay::SpeedDisplay()
    : Module("speed", "Speed", "Your speed in blocks per second.", Category::Hud) {
    SetDefaultLayout(ColumnRight(0.30f));
}

const char* SpeedDisplay::IconSvg() const {
    // Líneas de velocidad y una flecha hacia adelante.
    return "M3 8h9 M2 12h12 M4 16h8 M15 6.5l5 5.5-5 5.5";
}

ImVec2 SpeedDisplay::OnRenderHud(const HudFrame& frame) {
    sdk::player::Snapshot p;
    const bool ok = PlayerThisFrame(p);
    if (!ok && !frame.editing) {
        lastTick_ = -1;
        return ImVec2(0, 0);
    }
    if (ok && p.ticks != lastTick_) {
        // VELOCIDAD EXACTA: el juego mueve al jugador en "ticks" (20 por
        // segundo) y guarda dónde estaba en el tick anterior. Lo que te moviste
        // en ese tick x 20 = bloques por segundo. (Antes medíamos con el reloj
        // cada 100 ms y a veces caían 1, 2 o 3 ticks: el número saltaba.)
        lastTick_ = p.ticks;
        const float dx = p.eye[0] - p.eyeOld[0];
        const float dy = vertical_ ? p.eye[1] - p.eyeOld[1] : 0.0f;
        const float dz = p.eye[2] - p.eyeOld[2];
        float speed = std::sqrt(dx * dx + dy * dy + dz * dz) * 20.0f;
        if (!std::isfinite(speed) || speed > 200.0f) speed = 0.0f;  // teletransporte
        samples_[sampleIndex_] = speed;
        sampleIndex_ = (sampleIndex_ + 1) % kSamples;
        // Promedio de los últimos 5 ticks (un cuarto de segundo): estable pero rápido.
        float sum = 0.0f;
        for (float v : samples_) sum += v;
        shown_ = sum / static_cast<float>(kSamples);
    }
    char text[32];
    const float speed = ok ? shown_ : 4.32f;
    if (unit_ == 1) {
        std::snprintf(text, sizeof(text), "%.1f km/h", speed * 3.6f);
    } else {
        std::snprintf(text, sizeof(text), "%.2f m/s", speed);
    }
    return rh::TextBox(frame.draw, rh::Measure(frame.scale), frame.pos, text, 58.0f);
}

bool SpeedDisplay::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Unit", &unit_, {"m/s", "km/h"});
    changed |= ui::Toggle("Include vertical", &vertical_, "(falling and jumping)");
    ui::Hint("1 block = 1 meter.");
    return changed;
}

void SpeedDisplay::SaveSettings(nlohmann::json& out) const {
    out["vertical"] = vertical_;
    out["unit"] = unit_;
}

void SpeedDisplay::LoadSettings(const nlohmann::json& in) {
    vertical_ = in.value("vertical", false);
    unit_ = std::clamp(in.value("unit", 0), 0, 1);
}

// =============================================================================
//  Ping
// =============================================================================

PingDisplay::PingDisplay()
    : Module("ping", "Ping", "Your ping to the server (ms). Local worlds have no ping.", Category::Hud) {
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
        std::snprintf(text, sizeof(text), "%s", frame.editing && !showInLocal_ ? "42 ms" : Tr("Local world"));
    } else {
        return ImVec2(0, 0);
    }
    if (ping < 0 || !colorize_) {
        return rh::TextBox(frame.draw, rh::Measure(frame.scale), frame.pos, text, 58.0f);
    }
    // Con color: verde (< 80 ms), amarillo (< 150 ms) o rojo.
    const rh::Metrics m = rh::Measure(frame.scale);
    const float width = std::floor(std::max(rh::TextSize(m, text).x + 10.0f * m.px, 58.0f * m.px));
    const ImVec2 max(frame.pos.x + width, frame.pos.y + std::floor(rh::LineBoxHeight(m)));
    rh::Box(frame.draw, m, frame.pos, max);
    const ImU32 color = ping < 80 ? IM_COL32(110, 230, 110, 255) : ping < 150 ? IM_COL32(240, 210, 80, 255) : IM_COL32(240, 90, 80, 255);
    rh::CenteredText(frame.draw, m, frame.pos, max, text, color);
    return ImVec2(width, max.y - frame.pos.y);
}

bool PingDisplay::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Color by ping", &colorize_, "(green, yellow, red)");
    changed |= ui::Toggle("\"Local world\" without a server", &showInLocal_);
    return changed;
}

void PingDisplay::SaveSettings(nlohmann::json& out) const {
    out["showInLocal"] = showInLocal_;
    out["colorize"] = colorize_;
}

void PingDisplay::LoadSettings(const nlohmann::json& in) {
    showInLocal_ = in.value("showInLocal", false);
    colorize_ = in.value("colorize", true);
}

}  // namespace luma::modules::hud
