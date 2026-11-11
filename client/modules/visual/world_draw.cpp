// =============================================================================
//  world_draw.cpp - Implementación de Contorno de bloque e Hitboxes
// =============================================================================
#include "modules/visual/world_draw.h"

#include "render/icons.h"

#include <algorithm>
#include <cmath>

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
    : Module("block_outline", "Block outline",
             "Changes the outline of the block you look at: its color and an optional fill.", Category::Visual) {}

const char* BlockOutline::IconSvg() const {
    return render::icons::kBox;
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
    s.fillMaterial = fillMaterial_;
    s.rainbow = rainbow_;
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
    changed |= ui::ColorAlpha("Line color", line_);
    changed |= ui::Toggle("Rainbow", &rainbow_, "(keeps changing color)");
    changed |= ui::Toggle("Fill", &fill_);
    if (fill_) {
        changed |= ui::ColorAlpha("Fill color", fillColor_);
        // No sabemos con certeza qué "material" del juego dibuja mejor una cara
        // de color liso: si con uno no se ve, prueba otro.
        changed |= ui::Choice("Fill method", &fillMaterial_, {"A", "B", "C", "D"});
        ui::Hint("If the fill doesn't show or flickers, try another method.");
    }
    ui::Hint("Drawn inside the world: walls cover it, like the game's outline.");
    if (changed) {
        Push();
    }
    return changed;
}

void BlockOutline::SaveSettings(nlohmann::json& out) const {
    SaveColor(out, "line", line_);
    out["fill"] = fill_;
    SaveColor(out, "fillColor", fillColor_);
    out["fillMaterial"] = fillMaterial_;
    out["rainbow"] = rainbow_;
}

void BlockOutline::LoadSettings(const nlohmann::json& in) {
    LoadColor(in, "line", line_);
    fill_ = in.value("fill", false);
    LoadColor(in, "fillColor", fillColor_);
    fillMaterial_ = std::clamp(in.value("fillMaterial", 0), 0, sdk::draw3d::kFillMaterialCount - 1);
    rainbow_ = in.value("rainbow", false);
    Push();
}

// =============================================================================
//  Hitboxes
// =============================================================================

Hitboxes::Hitboxes()
    : Module("hitboxes", "Hitboxes",
             "Shows the hitbox of nearby entities, like Java's F3+B. Walls cover them.",
             Category::Visual) {}

const char* Hitboxes::IconSvg() const {
    return render::icons::kSquareDashed;
}

bool Hitboxes::SdkReady() const {
    return sdk::draw3d::Available();
}

void Hitboxes::Push() const {
    sdk::draw3d::HitboxSettings s;
    s.enabled = IsEnabled();
    s.players = players_;
    s.playerColor = ToColor(playerColor_);
    s.mobs = mobs_;
    s.mobColor = ToColor(mobColor_);
    s.items = items_;
    s.itemColor = ToColor(itemColor_);
    s.maxDistance = maxDistance_;
    s.showSelf = showSelf_;
    s.rainbow = rainbow_;
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
    changed |= ui::Toggle("Rainbow", &rainbow_, "(all boxes keep changing color)");
    ui::Section("Players");
    changed |= ui::Toggle("Show players", &players_);
    if (players_) {
        changed |= ui::ColorAlpha("Player color", playerColor_);
        changed |= ui::Toggle("Your own box", &showSelf_, "(in third person)");
    }
    ui::Section("Entities");
    changed |= ui::Toggle("Show mobs", &mobs_, "(animals, monsters...)");
    if (mobs_) {
        changed |= ui::ColorAlpha("Mob color", mobColor_);
    }
    changed |= ui::Toggle("Show items and orbs", &items_);
    if (items_) {
        changed |= ui::ColorAlpha("Item color", itemColor_);
    }
    ui::Section("Range");
    changed |= ui::Slider("Distance", &maxDistance_, 8.0f, 96.0f, "%.0f blocks");
    if (changed) {
        Push();
    }
    return changed;
}

void Hitboxes::SaveSettings(nlohmann::json& out) const {
    out["players"] = players_;
    SaveColor(out, "playerColor", playerColor_);
    out["mobs"] = mobs_;
    SaveColor(out, "mobColor", mobColor_);
    out["items"] = items_;
    SaveColor(out, "itemColor", itemColor_);
    out["maxDistance"] = maxDistance_;
    out["showSelf"] = showSelf_;
    out["rainbow"] = rainbow_;
}

void Hitboxes::LoadSettings(const nlohmann::json& in) {
    players_ = in.value("players", true);
    LoadColor(in, "playerColor", playerColor_);
    mobs_ = in.value("mobs", true);
    LoadColor(in, "mobColor", mobColor_);
    items_ = in.value("items", in.value("includeItems", false));
    LoadColor(in, "itemColor", itemColor_);
    // Antes los colores por defecto eran amarillo y celeste; ahora es blanco.
    // Si todavía tenías los viejos (nunca los cambiaste), pasan a blanco.
    auto oldDefault = [](const float* c, float r, float g, float b) {
        return std::fabs(c[0] - r) < 0.01f && std::fabs(c[1] - g) < 0.01f && std::fabs(c[2] - b) < 0.01f;
    };
    if (oldDefault(mobColor_, 1.0f, 0.85f, 0.3f)) {
        mobColor_[0] = mobColor_[1] = mobColor_[2] = 1.0f;
    }
    if (oldDefault(itemColor_, 0.5f, 0.8f, 1.0f)) {
        itemColor_[0] = itemColor_[1] = itemColor_[2] = 1.0f;
    }
    maxDistance_ = std::clamp(in.value("maxDistance", 32.0f), 8.0f, 96.0f);
    showSelf_ = in.value("showSelf", false);
    rainbow_ = in.value("rainbow", false);
    Push();
}

// =============================================================================
//  Bordes de chunk
// =============================================================================

ChunkBorders::ChunkBorders()
    : Module("chunk_borders", "Chunk borders",
             "Shows chunk borders (16x16 block columns), like Java's F3+G.", Category::Visual) {}

const char* ChunkBorders::IconSvg() const {
    return render::icons::kGrid;
}

bool ChunkBorders::SdkReady() const {
    return sdk::draw3d::Available();
}

void ChunkBorders::Push() const {
    sdk::draw3d::ChunkBorderSettings s;
    s.enabled = IsEnabled();
    s.inner = ToColor(inner_);
    s.corner = ToColor(corner_);
    s.neighbor = ToColor(neighbor_);
    s.neighbors = neighbors_;
    s.range = range_;
    s.rainbow = rainbow_;
    sdk::draw3d::SetChunkBorders(s);
}

void ChunkBorders::OnEnable() {
    Push();
}

void ChunkBorders::OnDisable() {
    Push();
}

bool ChunkBorders::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Rainbow", &rainbow_, "(the lines keep changing color)");
    changed |= ui::ColorAlpha("Chunk walls", inner_);
    changed |= ui::ColorAlpha("Chunk corners", corner_);
    changed |= ui::Toggle("Neighbor corners", &neighbors_);
    if (neighbors_) {
        changed |= ui::ColorAlpha("Neighbor color", neighbor_);
    }
    changed |= ui::SliderInt("Height", &range_, 8, 64, "%d blocks");
    ui::Hint("Drawn inside the world: walls cover them.");
    if (changed) {
        Push();
    }
    return changed;
}

void ChunkBorders::SaveSettings(nlohmann::json& out) const {
    SaveColor(out, "inner", inner_);
    SaveColor(out, "corner", corner_);
    SaveColor(out, "neighbor", neighbor_);
    out["neighbors"] = neighbors_;
    out["range"] = range_;
    out["rainbow"] = rainbow_;
}

void ChunkBorders::LoadSettings(const nlohmann::json& in) {
    LoadColor(in, "inner", inner_);
    LoadColor(in, "corner", corner_);
    LoadColor(in, "neighbor", neighbor_);
    neighbors_ = in.value("neighbors", true);
    range_ = std::clamp(in.value("range", 24), 8, 64);
    rainbow_ = in.value("rainbow", false);
    Push();
}

}  // namespace luma::modules::visual
