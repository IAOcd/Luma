// =============================================================================
//  cape_model.h - La capa 3D de Luma (ver cape_model.cpp)
// =============================================================================
#pragma once

#include <string>

namespace luma::cosmetics::model {

// Guarda las imágenes de las capas en %APPDATA%\Luma\capes\ (hilo principal
// de Luma, al arrancar). El juego las carga desde ahí como texturas.
void PrepareFiles();
// Empieza / deja de dibujar (se engancha a sdk/draw3d).
void Start();
void Stop();
// La capa que llevas ("" = ninguna).
void SetCape(const std::string& id);

}  // namespace luma::cosmetics::model
