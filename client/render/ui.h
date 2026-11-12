// =============================================================================
//  ui.h - Controles para las opciones de los módulos
// =============================================================================
//  Los mismos controles "de vidrio" del menú (interruptores tipo píldora,
//  selectores segmentados, deslizadores, colores...), para que las opciones
//  de cada módulo se vean igual que el resto de Luma, tanto en su página del
//  menú como en el panel del editor de HUD.
//
//  Cada control dibuja una fila "Etiqueta ......... control" y devuelve true
//  si el usuario cambió el valor (para guardar la config).
//
//      bool changed = false;
//      ui::Section("Texto");
//      changed |= ui::Toggle("Mostrar segundos", &seconds_);
//      changed |= ui::Choice("Formato", &format_, {"24 h", "12 h"});
//      changed |= ui::Slider("Tamaño", &size_, 1.0f, 10.0f, "%.0f");
//      return changed;
//
//  Implementados en render/menu.cpp (usan sus widgets y animaciones).
// =============================================================================
#pragma once

#include <initializer_list>

namespace luma::render::ui {

// Ancho de la columna de las etiquetas, en tamaños de letra (15 por
// defecto; el panel del editor de HUD, más angosto, usa menos).
void SetLabelColumn(float fontSizes);

// Título de un grupo de opciones (una línea con texto).
void Section(const char* title);

// Texto de ayuda tenue, en varias líneas si hace falta.
void Hint(const char* text);

bool Toggle(const char* label, bool* value, const char* hint = nullptr);

// Varias opciones excluyentes. Con hasta 4 cortas, un control segmentado;
// con más, una lista desplegable.
bool Choice(const char* label, int* index, std::initializer_list<const char*> options);

bool Slider(const char* label, float* value, float min, float max, const char* format = "%.1f");
bool SliderInt(const char* label, int* value, int min, int max, const char* format = "%d");

// Color RGB (3 floats) o RGBA (4 floats).
bool Color(const char* label, float* rgb);
bool ColorAlpha(const char* label, float* rgba);

// Caja de texto de una línea.
bool Text(const char* label, char* buffer, int size, const char* placeholder = "");

// A key for something of a module (click, then press the key; right click
// removes it). Saved with the module's settings.
bool Key(const char* label, int* key, const char* hint = nullptr);

// Botón (en su propia fila, a la derecha de la etiqueta si la hay).
bool Button(const char* label, const char* rowLabel = nullptr);

}  // namespace luma::render::ui
