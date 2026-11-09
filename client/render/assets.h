// =============================================================================
//  assets.h - Imágenes de Luma (el logo)
// =============================================================================
//  El logo de Luma viaja DENTRO de Luma.dll (client/assets/luma.rc). Al cargar:
//    1. Decode(): se decodifica el PNG con WIC (el decodificador de imágenes
//       que trae Windows) y se convierte en una "máscara": solo la forma de
//       la L, en blanco con transparencia. Así se puede dibujar de cualquier
//       color (marfil sobre vidrio oscuro, carbón sobre vidrio claro).
//       Se hace en el hilo principal de Luma.
//    2. UploadPending(): en el hilo de render, se sube a la GPU como textura.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::assets {

// Decodificar el logo (hilo principal de Luma, una vez).
bool Decode();

// Subir a la GPU lo que esté decodificado y todavía no subido (hilo de render).
void UploadPending();

// La textura del logo (0 si no está) y su proporción ancho / alto.
ImTextureID Logo();
float LogoAspect();

// Dibuja el logo dentro de un cuadrado de lado "size" (o una L de respaldo si
// la textura no está), centrado en "center", del color indicado.
void DrawLogo(ImDrawList* draw, ImVec2 center, float size, ImU32 color);

}  // namespace luma::render::assets
