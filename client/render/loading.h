// =============================================================================
//  loading.h - Pantalla de carga de Luma
// =============================================================================
//  Al inyectar Luma, mientras el hilo principal prepara todo (cargar la
//  configuración, escanear las firmas del juego, encontrar el objeto de la
//  ventana...), se muestra una tarjeta con el logo, el paso actual y una
//  barra de progreso. Al terminar se desvanece.
//
//  Es solo visual: el juego sigue funcionando normalmente detrás.
//  Se puede llamar desde cualquier hilo (tiene su propio candado).
// =============================================================================
#pragma once

namespace luma::render::loading {

// Mostrar la pantalla de carga.
void Begin();

// Cambiar el texto del paso actual y el progreso (0..1).
void SetStep(const char* text, float progress);

// Terminar: la pantalla se desvanece y desaparece.
void Finish();

// ¿Hay que dibujarla? (Atómico, sin candado: lo usa el camino rápido.)
bool IsVisible();

// Dibujarla (hilo de render, dentro de un frame de ImGui).
void Render();

}  // namespace luma::render::loading
