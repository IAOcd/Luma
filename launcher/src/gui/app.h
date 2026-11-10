// =============================================================================
//  app.h - La interfaz del launcher (lo que se dibuja en la ventana)
// =============================================================================
//  window.cpp crea la ventana y el "lienzo" (Direct3D 11) y llama a estas
//  funciones. Acá vive todo lo visual: barra lateral, inicio, JUGAR...
// =============================================================================
#pragma once

#include <windows.h>

namespace luma::launcher::gui {

// Una vez, con la ventana ya creada. "scale" = escala de Windows (1 = 100%).
void AppInit(HWND hwnd, float scale);
// La escala cambió (otra pantalla, o la cambiaste en Windows): rehacer fuentes.
void AppSetScale(float scale);
// ¿Hay que reconstruir las fuentes antes del próximo frame? (lo pide AppSetScale)
bool AppFontsDirty();
void AppRebuildFonts();
// Dibujar un frame (entre ImGui::NewFrame y ImGui::Render).
void AppFrame();
void AppShutdown();

// ¿El punto (en píxeles de la ventana) es la barra de título "arrastrable"?
// (no lo es si cae sobre un botón). Lo usa window.cpp para que Windows deje
// mover la ventana desde ahí.
bool AppIsCaption(int x, int y);

}  // namespace luma::launcher::gui
