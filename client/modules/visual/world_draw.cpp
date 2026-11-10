// =============================================================================
//  world_draw.cpp - Implementación de Contorno de bloque e Hitboxes
// =============================================================================
#include "modules/visual/world_draw.h"

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
    s.fillMaterial = fillMaterial_;
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
        // No sabemos con certeza qué "material" del juego dibuja mejor una cara
        // de color liso: si con uno no se ve, prueba otro.
        changed |= ui::Choice("Método de relleno", &fillMaterial_, {"A", "B", "C", "D"});
        ui::Hint("Si el relleno no se ve o parpadea, prueba otro método.");
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
    out["fillMaterial"] = fillMaterial_;
}

void BlockOutline::LoadSettings(const nlohmann::json& in) {
    LoadColor(in, "line", line_);
    fill_ = in.value("fill", false);
    LoadColor(in, "fillColor", fillColor_);
    fillMaterial_ = std::clamp(in.value("fillMaterial", 0), 0, sdk::draw3d::kFillMaterialCount - 1);
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
    s.players = players_;
    s.playerColor = ToColor(playerColor_);
    s.mobs = mobs_;
    s.mobColor = ToColor(mobColor_);
    s.items = items_;
    s.itemColor = ToColor(itemColor_);
    s.maxDistance = maxDistance_;
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
    ui::Section("Jugadores");
    changed |= ui::Toggle("Mostrar jugadores", &players_);
    if (players_) {
        changed |= ui::ColorAlpha("Color de jugadores", playerColor_);
        changed |= ui::Toggle("Tu propia caja", &showSelf_, "(en tercera persona)");
    }
    ui::Section("Entidades");
    changed |= ui::Toggle("Mostrar mobs", &mobs_, "(animales, monstruos...)");
    if (mobs_) {
        changed |= ui::ColorAlpha("Color de mobs", mobColor_);
    }
    changed |= ui::Toggle("Mostrar ítems y orbes", &items_);
    if (items_) {
        changed |= ui::ColorAlpha("Color de ítems", itemColor_);
    }
    ui::Section("Alcance");
    changed |= ui::Slider("Distancia", &maxDistance_, 8.0f, 96.0f, "%.0f bloques");
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
    Push();
}

// =============================================================================
//  Bordes de chunk
// =============================================================================

ChunkBorders::ChunkBorders()
    : Module("chunk_borders", "Bordes de chunk",
             "Muestra los bordes de los chunks (columnas de 16x16 bloques), como el F3+G de Java.", Category::Visual) {}

const char* ChunkBorders::IconSvg() const {
    return "M4 4h16v16H4z M12 4v16 M4 12h16";
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
    changed |= ui::ColorAlpha("Paredes del chunk", inner_);
    changed |= ui::ColorAlpha("Esquinas del chunk", corner_);
    changed |= ui::Toggle("Esquinas de los vecinos", &neighbors_);
    if (neighbors_) {
        changed |= ui::ColorAlpha("Color de los vecinos", neighbor_);
    }
    changed |= ui::SliderInt("Altura", &range_, 8, 64, "%d bloques");
    ui::Hint("Se dibujan dentro del mundo: las paredes los tapan.");
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
}

void ChunkBorders::LoadSettings(const nlohmann::json& in) {
    LoadColor(in, "inner", inner_);
    LoadColor(in, "corner", corner_);
    LoadColor(in, "neighbor", neighbor_);
    neighbors_ = in.value("neighbors", true);
    range_ = std::clamp(in.value("range", 24), 8, 64);
    Push();
}

}  // namespace luma::modules::visual
