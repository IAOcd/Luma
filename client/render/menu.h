// =============================================================================
//  menu.h - El menú de Luma (lo que se ve dentro de la ventana)
// =============================================================================
//  HILOS: todo con luma::StateMutex() tomado.
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

// Dibuja la ventana del menú. Se llama entre ImGui::NewFrame y ImGui::Render.
void Draw(const RenderInfo& info);

// El menú se acaba de abrir / cerrar (animaciones y sonidos).
void OnOpened();
void OnClosed();

// ¿El menú ya se cerró pero todavía se está desvaneciendo? Mientras sea true,
// el overlay lo sigue dibujando (sin recibir input). Se puede llamar sin el
// candado (es atómico).
bool IsAnimatingClose();

// --- Asignación de teclas ---------------------------------------------------------
// Al hacer clic en el botón de tecla de un módulo, el menú queda "esperando
// una tecla". La siguiente tecla que pulses (llega por la ventana del juego)
// se asigna. Esc cancela; Retroceso o Supr dejan el módulo sin tecla.
bool IsCapturingKey();
void OnKeyCaptured(int virtualKey);

}  // namespace luma::render::menu
