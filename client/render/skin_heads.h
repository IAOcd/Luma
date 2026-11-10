// =============================================================================
//  skin_heads.h - Caras de skins como imágenes (menú radial, Closet, Tab)
// =============================================================================
//  La "cara" de una skin de Minecraft es el cuadrado de 8x8 de la cabeza
//  (frente) más la segunda capa (pelo, sombrero) encima. La recortamos y la
//  agrandamos a 128x128 copiando cada píxel en un bloque de 16x16: así se ve
//  nítida aunque ImGui la dibuje con suavizado.
//
//  Las imágenes se suben a la GPU en el hilo de render (cuando se piden).
// =============================================================================
#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>

namespace luma::render::skinheads {

// La skin de TU jugador (cualquier hilo: la sube el render después).
void SetOwn(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height);
// Lo mismo, desde un PNG (al cargar Luma, la última skin guardada). Hilo de Luma.
void LoadOwnFromFile(const std::wstring& path);
// Tu cara (0 si todavía no se leyó tu skin). Solo en el hilo de render.
ImTextureID Own();

// La cara de un PNG de skin (la biblioteca de skins). Solo en el hilo de
// render. Devuelve 0 mientras se carga (el archivo lo lee Poll, en el hilo de
// Luma: leer archivos con WIC necesita COM, que no queremos iniciar en los
// hilos del juego).
ImTextureID FromFile(const std::wstring& path);
// En el hilo principal de Luma, cada vuelta: lee los PNG pedidos.
void Poll();

}  // namespace luma::render::skinheads
