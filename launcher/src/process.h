// =============================================================================
//  process.h - Herramientas para encontrar y examinar el proceso del juego
// =============================================================================
//  Un "proceso" es un programa en ejecución. Cada uno tiene un número único,
//  el PID (Process ID). Para hacerle algo a un proceso (leer su información,
//  inyectarle una DLL), primero hay que "abrirlo" y obtener un HANDLE:
//  una especie de llave que Windows nos da y que hay que devolver al terminar.
// =============================================================================
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luma::launcher {

// --- UniqueHandle ------------------------------------------------------------
// Envoltorio que devuelve (cierra) un HANDLE automáticamente cuando deja de
// usarse. Así es imposible olvidarse de llamar a CloseHandle, incluso si una
// función termina antes de tiempo por un error. A esta técnica se la llama
// RAII ("la vida del recurso va atada a la vida del objeto").
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { Reset(); }

    // No se puede copiar (dos copias intentarían cerrar el mismo handle),
    // pero sí "mover" (pasarle la propiedad a otra variable).
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    HANDLE Get() const { return handle_; }
    bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void Reset() {
        if (Valid()) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

// --- Funciones ----------------------------------------------------------------

// Devuelve los PIDs de todos los procesos cuyo ejecutable se llama "exeName".
std::vector<DWORD> FindProcessesByName(std::wstring_view exeName);

// Ruta completa del ejecutable de un proceso abierto.
// "std::optional" = "puede que haya un valor o puede que no": si falla,
// devuelve std::nullopt en lugar de una ruta.
std::optional<std::filesystem::path> GetProcessImagePath(HANDLE process);

// ¿El proceso corre dentro de un AppContainer (el "sandbox" de las apps UWP)?
// nullopt si no se pudo averiguar.
std::optional<bool> IsAppContainer(HANDLE process);

// ¿El proceso ya tiene cargada una DLL con este nombre (por ejemplo "Luma.dll")?
// nullopt si no se pudo averiguar.
std::optional<bool> IsModuleLoaded(DWORD pid, std::wstring_view moduleName);

// Texto en español (o el idioma de Windows) que describe un código de error
// de Windows, por ejemplo 5 -> "Acceso denegado".
std::string ErrorMessage(DWORD error);

// Convierte texto "ancho" (UTF-16, el que usa Windows) a UTF-8 para mostrarlo
// en la consola.
std::string ToUtf8(std::wstring_view text);

}  // namespace luma::launcher
