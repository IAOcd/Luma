// =============================================================================
//  paths.h - Carpetas de datos de Luma
// =============================================================================
//  Todo lo que Luma guarda en disco vive en %APPDATA%\Luma\ :
//    %APPDATA%\Luma\logs\        -> archivos de log
//    (más adelante: config, perfiles, waypoints, benchmarks)
//
//  %APPDATA% es una carpeta de Windows para datos de programas del usuario,
//  normalmente C:\Users\<tu usuario>\AppData\Roaming.
// =============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace luma::paths {

// Devuelve %APPDATA%\Luma. Si Windows no puede darnos %APPDATA% (muy raro),
// devuelve una ruta vacía y quien llama decide qué hacer.
//
// "std::filesystem::path" es el tipo de C++ para representar rutas de
// archivos; se encarga de las barras "\" y de los caracteres especiales.
std::filesystem::path DataDir();

// Devuelve %APPDATA%\Luma\logs (o vacío si DataDir() falla).
std::filesystem::path LogsDir();

// Convierte una ruta a texto UTF-8 para escribirla en el log.
// (path.string() de C++ puede lanzar un error si la ruta tiene caracteres
// que la configuración regional de Windows no sabe representar; esta función
// nunca falla.)
std::string ToUtf8(const std::filesystem::path& path);

}  // namespace luma::paths
