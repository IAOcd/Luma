// =============================================================================
//  crosshair.h - Módulo "Mira personalizada"
// =============================================================================
//  Tu propia mira en el centro de la pantalla, de dos tipos:
//    - Forma: cruz, cruz con punto, punto o círculo (largo, grosor, hueco).
//    - Pintada: la dibujas píxel por píxel en un editor (8x8, 16x16, 32x32 o
//      64x64), con los colores que quieras.
//  Colores: los originales, "Rainbow" (todo cambia de color) o "Rainbow en
//  onda" (cada píxel con un tono distinto que se va moviendo).
//
//  Esconde la mira del juego (sdk/game_crosshair.h) mientras está activa.
//  Se esconde sola en los menús del juego (inventario, chat, pausa).
//
//  RENDIMIENTO: la mira pintada se dibuja como "tiras" de píxeles iguales
//  seguidos (se calculan al editar, no cada frame).
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "modules/module.h"

namespace luma::modules::visual {

class Crosshair : public Module {
public:
    Crosshair();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool IsHudElement() const override { return false; }  // siempre en el centro
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    struct Run {  // una tira horizontal de píxeles del mismo color
        std::uint16_t y, x0, x1;
        std::uint32_t color;  // como IM_COL32
    };
    void DrawShape(ImDrawList* draw, ImVec2 center, float u) const;
    void DrawPainted(ImDrawList* draw, ImVec2 center, float u) const;
    void RebuildRuns();
    void Resize(int size);
    void ShapeToPixels();
    bool DrawEditor();
    std::uint32_t Tinted(std::uint32_t color, float offset) const;

    // Tipo y forma
    int mode_ = 0;           // 0 forma, 1 pintada
    int style_ = 0;          // 0 cruz, 1 cruz con punto, 2 punto, 3 círculo
    float length_ = 5.0f;    // largo de cada brazo (píxeles de HUD)
    float thickness_ = 1.0f;
    float gap_ = 2.0f;       // hueco en el centro
    float color_[4] = {1.0f, 1.0f, 1.0f, 0.95f};
    bool outline_ = true;    // contorno negro para que se vea sobre fondos claros
    // Color
    int colorMode_ = 0;      // 0 original, 1 rainbow, 2 rainbow en onda, 3 album colors
    float rainbowSpeed_ = 0.25f;
    // Mira pintada
    int gridSize_ = 16;      // 8, 16, 32 o 64
    float pixelScale_ = 1.0f;  // píxeles de HUD por cuadradito
    std::vector<std::uint32_t> pixels_;  // gridSize_ x gridSize_, IM_COL32 (alfa 0 = vacío)
    std::vector<Run> runs_;
    // Editor
    float brush_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool mirrorX_ = true;
    bool mirrorY_ = true;
    // General
    bool hideGame_ = true;   // esconder la mira del juego
    bool hideInMenus_ = true;
    bool firstPersonOnly_ = true;  // hidden in third person and with the freelook (you aim with the camera, not the player)
};

}  // namespace luma::modules::visual
