// =============================================================================
//  post_processing.h - Módulo "Post-procesado": un editor de fotos para el juego
// =============================================================================
//  Cambia cómo se ve Minecraft, como los filtros de un editor de fotos:
//  exposición, brillo, contraste, saturación, intensidad, gamma, temperatura,
//  tinte, viñeta y nitidez. Con estilos listos (Vívido, Cálido, Cine...).
//
//  CÓMO: un shader (programa de la GPU) recorre cada píxel del frame del
//  juego y le cambia el color, justo antes de mostrarlo (render/gpu.h,
//  RecordPost). La interfaz del juego (barra rápida, chat) también cambia,
//  porque llega ya mezclada en la misma imagen; el menú y el HUD de Luma no,
//  porque se dibujan después.
//
//  COSTO: una pasada de pantalla completa en la GPU por frame (medible con
//  el benchmark). Desactivado, no cuesta nada.
// =============================================================================
#pragma once

#include "modules/module.h"
#include "render/gpu.h"

namespace luma::modules::visual {

class PostProcessing : public Module {
public:
    PostProcessing();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Push() const;
    render::gpu::PostParams params_;
    int preset_ = 0;
};

}  // namespace luma::modules::visual
