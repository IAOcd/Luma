// =============================================================================
//  logger.h - Registro de mensajes (log) de Luma
// =============================================================================
//  El log es un archivo de texto donde Luma anota lo que va haciendo:
//  "cargado", "hook instalado", "firma no encontrada", etc. Es nuestra
//  herramienta principal para entender qué pasó si algo falla dentro del
//  juego, donde no podemos usar un depurador fácilmente.
//
//  Uso:
//      luma::log::Info("Luma {} cargado", luma::kVersion);
//      luma::log::Warn("No se encontró la firma {}", nombre);
//      luma::log::Error("Fallo al crear el hook: {}", codigo);
//
//  Las llaves {} se reemplazan por los valores que vienen después, en orden
//  (esto lo hace std::format, parte de C++20).
//
//  RENDIMIENTO: cada mensaje arma un texto y lo escribe en disco al instante.
//  Eso cuesta del orden de microsegundos: está bien para eventos (cargar,
//  descargar, errores), pero NUNCA debe llamarse en cada frame o en cada tick.
// =============================================================================
#pragma once

#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace luma::log {

// "enum class" = una lista cerrada de opciones con nombre.
enum class Level { Info, Warn, Error };

// Abre un archivo de log nuevo en "logDir" (creando la carpeta si hace
// falta) y borra los más viejos para que nunca haya más de 10.
// Si "withConsole" es true, también abre una ventana de consola donde se ve
// el log en vivo.
// Devuelve false si no pudo abrir el archivo (Luma sigue funcionando igual,
// solo que sin log en disco).
bool Init(const std::filesystem::path& logDir, bool withConsole);

// Cierra el archivo y la consola. Después de esto, los mensajes se ignoran.
void Shutdown();

// Escribe una línea ya armada. Normalmente se usan Info/Warn/Error de abajo.
void Write(Level level, std::string_view message);

// Ruta del archivo de log actual (vacía si no hay).
const std::filesystem::path& CurrentFile();

// --- Atajos con formato -------------------------------------------------------
// "template" permite que una misma función acepte argumentos de cualquier
// tipo y cantidad. std::format_string además revisa EN TIEMPO DE COMPILACIÓN
// que la cantidad de {} coincida con la de argumentos: un error ahí no llega
// nunca al juego.
template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace luma::log
