// =============================================================================
//  textures.h - Imágenes de archivo (PNG/JPG) como texturas para ImGui
// =============================================================================
//  Las imágenes de las noticias (y más adelante las skins) se leen del disco
//  con WIC (el decodificador de imágenes de Windows) y se suben a la GPU UNA
//  vez; después se dibujan cada frame sin costo extra.
// =============================================================================
#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;

namespace luma::launcher::gui::textures {

struct Texture {
    ImTextureID id{};
    int width = 0;
    int height = 0;
};

void SetDevice(ID3D11Device* device);
// La textura de ese archivo (se carga la primera vez que se pide). nullptr si
// el archivo no existe o no es una imagen válida.
// "cacheKey": si el archivo puede cambiar (la skin), pasar una clave distinta
// cada vez que cambie (por ejemplo, con la fecha del archivo) para recargarla.
const Texture* Get(const std::wstring& path, const std::wstring& cacheKey = {});
// Decodifica un archivo a RGBA en memoria (sin GPU). Para revisar píxeles.
bool DecodeFile(const std::wstring& path, std::vector<std::uint8_t>& rgba, unsigned& width, unsigned& height);
// Libera todas (al cerrar el launcher).
void ReleaseAll();

}  // namespace luma::launcher::gui::textures
