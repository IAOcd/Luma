// =============================================================================
//  version.h - Datos básicos de Luma
// =============================================================================
//  Un archivo ".h" (header o cabecera) contiene declaraciones que otros
//  archivos pueden incluir con #include. Aquí guardamos el nombre y la versión
//  para no escribirlos a mano en varios lugares.
// =============================================================================

// "#pragma once" evita que este archivo se incluya dos veces en la misma
// compilación (lo que causaría errores de "definido dos veces").
#pragma once

// Un "namespace" agrupa nombres bajo un prefijo. Todo lo nuestro vive dentro
// de "luma", así que esto se usa como luma::kName y nunca choca con nombres
// de Windows o de otras bibliotecas.
namespace luma {

// "inline constexpr" = una constante conocida al compilar, sin costo en
// tiempo de ejecución. La "k" inicial es una convención para constantes.
inline constexpr const char* kName = "Luma";
inline constexpr const char* kVersion = "0.1.0";

}  // namespace luma
