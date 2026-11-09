// =============================================================================
//  notifications.h - Avisos tipo "toast"
// =============================================================================
//  Mensajes cortos que aparecen abajo a la derecha unos segundos y
//  desaparecen solos: "Marca de agua activada", "Config dañada, se usó la de
//  fábrica", etc.
//
//  Se pueden crear desde cualquier hilo (Push tiene su propio candado).
//  Mientras haya avisos visibles, el overlay dibuja cada frame; cuando no
//  queda ninguno, vuelve al camino rápido (no dibuja nada).
// =============================================================================
#pragma once

#include <string>

namespace luma::notify {

enum class Kind { Info, Success, Warning, Error };

void Push(Kind kind, std::string text);

// ¿Hay algún aviso en pantalla? Sin candado (atómico).
bool HasActive();

// Dibuja los avisos (hilo de render, dentro de un frame de ImGui).
void Render();

// Borra todos (al descargar Luma).
void Clear();

}  // namespace luma::notify
