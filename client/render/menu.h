// =============================================================================
//  menu.h - Contenido del menú de Luma (lo que se ve dentro de la ventana)
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::render::menu {

// Datos del render para mostrar en el menú (rellenados por overlay.cpp).
struct RenderInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bufferCount = 0;
    const char* formatName = "";
};

// Aplica el estilo visual de Luma a ImGui. "scale" agranda todo en
// resoluciones altas (1.0 = 1080p).
void ApplyStyle(float scale);

// Dibuja la ventana del menú. Se llama entre ImGui::NewFrame y ImGui::Render.
void Draw(const RenderInfo& info);

}  // namespace luma::render::menu
