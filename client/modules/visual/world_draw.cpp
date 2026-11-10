// =============================================================================
//  world_draw.cpp - Implementación de Contorno de bloque e Hitboxes
// =============================================================================
#include "modules/visual/world_draw.h"

#include <algorithm>

#include "render/ui.h"

namespace luma::modules::visual {

namespace ui = render::ui;

namespace {

sdk::draw3d::Color ToColor(const float* c) {
    return sdk::draw3d::Color{c[0], c[1], c[2], c[3]};
}

void SaveColor(nlohmann::json& out, const char* key, const float* c) {
    out[key] = {c[0], c[1], c[2], c[3]};
}

void LoadColor(const nlohmann::json& in, const char* key, float* c) {
    if (in.contains(key) && in[key].is_array() && in[key].size() == 4) {
        for (int i = 0; i < 4; ++i) {
            c[i] = std::clamp(in[key][i].get<float>(), 0.0f, 1.0f);
        }
    }
}

}  // namespace

// =============================================================================
//  Contorno de bloque
// =============================================================================

BlockOutline::BlockOutline()
    : Module("block_outline", "Contorno de bloque",
             "Cambia el contorno del bloque que miras: su color y un relleno opcional.", Category::Visual) {}

const char* BlockOutline::IconSvg() const {
    return "M12 3l8 4.5v9L12 21l-8-4.5v-9z M12 12l8-4.5 M12 12v9 M12 12L4 7.5";
}

bool BlockOutline::SdkReady() const {
    return sdk::draw3d::Available();
}

void BlockOutline::Push() const {
    sdk::draw3d::OutlineSettings s;
    s.enabled = IsEnabled();
    s.line = ToColor(line_);
    s.fill = fill_;
    s.fillColor = ToColor(fillColor_);
    sdk::draw3d::SetOutline(s);
}

void BlockOutline::OnEnable() {
    Push();
}

void BlockOutline::OnDisable() {
    Push();
}

bool BlockOutline::DrawSettings() {
    bool changed = false;
    changed |= ui::ColorAlpha("Color de la línea", line_);
    changed |= ui::Toggle("Relleno", &fill_);
    if (fill_) {
        changed |= ui::ColorAlpha("Color del relleno", fillColor_);
    }
    ui::Hint("Se dibuja dentro del mundo: las paredes lo tapan, igual que el contorno del juego.");
    if (changed) {
        Push();
    }
    return changed;
}

void BlockOutline::SaveSettings(nlohmann::json& out) const {
    SaveColor(out, "line", line_);
    out["fill"] = fill_;
    SaveColor(out, "fillColor", fillColor_);
}

void BlockOutline::LoadSettings(const nlohmann::json& in) {
    LoadColor(in, "line", line_);
    fill_ = in.value("fill", false);
    LoadColor(in, "fillColor", fillColor_);
    Push();
}

// =============================================================================
//  Hitboxes
// =============================================================================

Hitboxes::Hitboxes()
    : Module("hitboxes", "Hitboxes",
             "Muestra la caja de golpe de las entidades cercanas, como el F3+B de Java. Las paredes la tapan.",
             Category::Visual) {}

const char* Hitboxes::IconSvg() const {
    return "M7 3h10v18H7z M7 9h10 M10 6h4";
}

bool Hitboxes::SdkReady() const {
    return sdk::draw3d::Available();
}

void Hitboxes::Push() const {
    sdk::draw3d::HitboxSettings s;
    s.enabled = IsEnabled();
    s.line = ToColor(line_);
    s.maxDistance = maxDistance_;
    s.includeItems = includeItems_;
    s.showSelf = showSelf_;
    sdk::draw3d::SetHitboxes(s);
}

void Hitboxes::OnEnable() {
    Push();
}

void Hitboxes::OnDisable() {
    Push();
}

bool Hitboxes::DrawSettings() {
    bool changed = false;
    changed |= ui::ColorAlpha("Color", line_);
    changed |= ui::Slider("Distancia", &maxDistance_, 8.0f, 96.0f, "%.0f bloques");
    changed |= ui::Toggle("Ítems tirados y orbes", &includeItems_);
    changed |= ui::Toggle("Tu propia caja", &showSelf_, "(en tercera persona)");
    if (changed) {
        Push();
    }
    return changed;
}

void Hitboxes::SaveSettings(nlohmann::json& out) const {
    SaveColor(out, "line", line_);
    out["maxDistance"] = maxDistance_;
    out["includeItems"] = includeItems_;
    out["showSelf"] = showSelf_;
}

void Hitboxes::LoadSettings(const nlohmann::json& in) {
    LoadColor(in, "line", line_);
    maxDistance_ = std::clamp(in.value("maxDistance", 32.0f), 8.0f, 96.0f);
    includeItems_ = in.value("includeItems", false);
    showSelf_ = in.value("showSelf", false);
    Push();
}

}  // namespace luma::modules::visual
