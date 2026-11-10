// =============================================================================
//  world_draw.h - Contorno de bloque e Hitboxes (Fase 8)
// =============================================================================
//  Los dos se dibujan DENTRO del mundo (sdk/draw3d.h): las paredes los tapan
//  igual que a cualquier bloque o jugador. Así no muestran nada que no veas
//  ya, y se pueden usar también en servidores.
//
//  - Contorno de bloque: reemplaza el contorno del bloque que miras por uno
//    con tu color (y relleno opcional).
//  - Hitboxes: la caja de cada entidad cercana (jugadores, mobs...), como el
//    F3+B de la edición Java.
// =============================================================================
#pragma once

#include "modules/module.h"
#include "sdk/draw3d.h"

namespace luma::modules::visual {

class BlockOutline : public Module {
public:
    BlockOutline();
    const char* IconSvg() const override;
    bool SdkReady() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Push() const;
    float line_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool fill_ = false;
    float fillColor_[4] = {1.0f, 1.0f, 1.0f, 0.15f};
    int fillMaterial_ = 0;
};

class Hitboxes : public Module {
public:
    Hitboxes();
    const char* IconSvg() const override;
    bool SdkReady() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Push() const;
    bool players_ = true;
    float playerColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool mobs_ = true;
    float mobColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool items_ = false;
    float itemColor_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float maxDistance_ = 32.0f;
    bool showSelf_ = false;
};

}  // namespace luma::modules::visual
