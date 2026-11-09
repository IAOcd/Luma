// =============================================================================
//  hud_editor.h - Editor de HUD: mover, escalar y alinear los elementos
// =============================================================================
//  Se abre desde el menú ("Editar HUD"). El menú se oculta y quedan los
//  elementos del HUD activos con un contorno:
//
//    - Arrastrar con el clic izquierdo: mover. Se "imanta" a los bordes y al
//      centro de la pantalla y a los bordes de los otros elementos (con una
//      línea guía). Mantén Shift para moverlo libre.
//    - Rueda del mouse sobre un elemento: cambiar su tamaño.
//    - Clic derecho: abrir su panel de opciones (tamaño, estilo propio y
//      todas las opciones del módulo), ahí mismo.
//    - Esc o "Listo": volver al menú.
//
//  Técnicamente es un "modo" del menú: el input sigue bloqueado para el juego
//  igual que con el menú abierto (por eso no cuesta nada con el menú cerrado).
//
//  HILOS: con luma::StateMutex() tomado.
// =============================================================================
#pragma once

namespace luma::render::hudeditor {

bool IsActive();
void Begin();
void End();

// Dibuja el editor (en lugar del menú). Dentro de un frame de ImGui.
void Draw();

}  // namespace luma::render::hudeditor
