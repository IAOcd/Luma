// =============================================================================
//  crosshair.h - Módulo "Mira personalizada"
// =============================================================================
//  Dibuja una mira propia en el centro de la pantalla: cruz, cruz con punto,
//  punto o círculo, con tamaño, grosor, separación, color y contorno.
//
//  LIMITACIÓN (honesta): Luma dibuja ENCIMA del juego; la mira original del
//  juego sigue ahí. Para esconderla hace falta tocar el render del juego
//  (Fase 8, con el SDK) o usar un paquete de recursos que la deje vacía.
//
//  Se esconde sola cuando estás en un menú del juego (inventario, chat,
//  pausa): Luma lo sabe porque el juego "suelta" el mouse.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::visual {

class Crosshair : public Module {
public:
    Crosshair();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool IsHudElement() const override { return false; }  // siempre en el centro
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int style_ = 0;          // 0 cruz, 1 cruz con punto, 2 punto, 3 círculo
    float length_ = 5.0f;    // largo de cada brazo (píxeles de HUD)
    float thickness_ = 1.0f;
    float gap_ = 2.0f;       // hueco en el centro
    float color_[4] = {1.0f, 1.0f, 1.0f, 0.95f};
    bool outline_ = true;    // contorno negro para que se vea sobre fondos claros
    bool hideInMenus_ = true;
};

}  // namespace luma::modules::visual
