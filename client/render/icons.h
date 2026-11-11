// =============================================================================
//  icons.h - Íconos SVG de Luma
// =============================================================================
//  Cada ícono es un texto SVG dibujado en una cuadrícula de 24x24:
//
//    - La mayoría vienen de Lucide (lucide.dev, licencia ISC): un set de
//      íconos de línea muy prolijo, el mismo estilo en todos. Están en
//      lucide_icons.inc, generado por un script (no se edita a mano).
//    - Algunos son propios de Luma (el logo), escritos como un "path" de SVG:
//      el mismo texto que va en <path d="..."/>. Ejemplo, una flecha:
//          "M15 5L8 12L15 19"   (mover a 15,5; línea a 8,12; línea a 15,19)
//
//  Si el texto empieza con "F", la forma se RELLENA (además de su trazo).
//
//  CÓMO SE DIBUJAN (detalle en icons.cpp): nanosvg convierte el SVG en
//  píxeles una sola vez por tamaño, con bordes suaves, y los guarda en una
//  textura compartida (un "atlas"). Dibujar después es pegar ese pedacito
//  con el color pedido: tan barato como una letra.
// =============================================================================
#pragma once

#include <imgui.h>

#include <cstdint>

namespace luma::render::icons {

// La "cabecera" que comparten todos los íconos de Lucide: cuadrícula 24x24,
// sin relleno, trazo de 2 con puntas y uniones redondas.
#define LUMA_LUCIDE(body)                                                                                 \
    "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' "    \
    "stroke='#fff' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>" body "</svg>"

#include "render/lucide_icons.inc"

// --- Íconos propios de Luma --------------------------------------------------
// El logo de Luma (la "L" orgánica), RELLENO. Calcado a mano del PNG del logo.
inline constexpr const char* kLumaLogo =
    "F M2.3 8.5 C2.3 5.8 4.2 4.8 6.2 4.8 C8.4 4.8 9.8 6.3 9.6 8.3 C9.4 10.3 8.3 11.6 8.4 13.3 "
    "C8.5 14.4 9.6 14.3 10.4 13.6 C11.6 12.4 12.8 11.6 14.4 11.6 C16.2 11.6 17.4 12.8 18.2 14.0 "
    "C18.6 14.5 19.0 14.2 19.6 14.0 C21.0 13.7 22.2 14.9 22.1 16.4 C22.0 18.3 20.4 19.4 18.6 18.4 "
    "C16.6 17.3 14.6 16.3 12.4 16.6 C10.0 16.9 8.6 18.6 5.8 18.6 C3.6 18.6 2.3 17.1 2.3 15.0 Z";
// El logo de Windows (cuatro ventanas inclinadas): Lucide no tiene marcas.
inline constexpr const char* kWindowsLogo =
    "F M4 5.5l7-1v7H4z M13 4.2l7-1v8.3h-7z M4 13h7v7l-7-1z M13 13h7v8.3l-7-1z";

// Dibuja un ícono centrado en "center", de lado "size" píxeles. "thickness" =
// grosor de línea en píxeles (0 = el normal de Lucide, 2/24 del tamaño).
void Draw(ImDrawList* draw, const char* svg, ImVec2 center, float size, ImU32 color, float thickness = 0.0f);

// --- Conexión con la GPU --------------------------------------------------------
// Luma.dll (DirectX 12) y el launcher (DirectX 11) suben texturas de forma
// distinta, así que cada uno le dice aquí cómo hacerlo. Sin esto, se usa un
// dibujante simple con líneas de ImGui.
using CreateFn = ImTextureID (*)(const std::uint8_t* rgba, unsigned width, unsigned height);
using UpdateFn = bool (*)(ImTextureID id, const std::uint8_t* rgba, unsigned width, unsigned height);
void SetBackend(CreateFn create, UpdateFn update);

// Sube a la GPU los íconos nuevos de este frame (si hubo). Llamar una vez por
// frame, justo antes de ImGui::Render().
void Flush();

// Olvida la textura y libera la memoria (al cerrar, antes de liberar la GPU).
void Shutdown();

}  // namespace luma::render::icons
