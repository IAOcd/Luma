# =============================================================================
#  Luma - CMakeLists.txt raíz
# =============================================================================
#  CMake es una herramienta que lee esta "receta" y genera un proyecto de
#  Visual Studio (.sln). Luego Visual Studio compila el código y produce
#  los archivos finales: Luma.dll y LumaLauncher.exe.
#
#  Este archivo define opciones comunes a todo el proyecto y luego "entra"
#  en las subcarpetas (launcher/ y client/), que tienen su propio CMakeLists.
# =============================================================================

# Versión mínima de CMake que necesitamos (3.24 tiene todo lo que usamos).
cmake_minimum_required(VERSION 3.24)

# Nombre del proyecto y lenguaje (CXX = C++).
project(Luma VERSION 0.6.0 LANGUAGES CXX)

# --- Solo Windows x64 --------------------------------------------------------
# Minecraft Bedrock (GDK) es un programa de 64 bits. Una DLL de 32 bits no
# puede cargarse dentro de un proceso de 64 bits, así que rechazamos cualquier
# otra configuración desde el principio con un mensaje claro.
if(NOT WIN32)
    message(FATAL_ERROR "Luma solo se compila en Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Luma debe compilarse en x64 (64 bits). Usa el preset 'x64'.")
endif()

# --- Estándar de C++ ---------------------------------------------------------
# C++20 es la versión del lenguaje que usamos. "REQUIRED" significa que si el
# compilador no la soporta, falla en vez de usar una versión vieja en silencio.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Runtime de C++ estático (/MT) --------------------------------------------
# Todo programa C++ usa una "biblioteca de runtime" (funciones básicas como
# manejar memoria o texto). Con /MT esa biblioteca queda DENTRO de Luma.dll en
# vez de depender de archivos externos. Ventaja: la DLL funciona sola, sin
# chocar con la versión del runtime que use el juego.
# Va ANTES de descargar las dependencias para que ellas usen lo mismo.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Carpeta de salida -------------------------------------------------------
# Todos los .exe y .dll terminan juntos en build/bin/Release (o Debug).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Dependencias externas ---------------------------------------------------
# MinHook, Dear ImGui y nlohmann/json se descargan automáticamente.
# Ver cmake/Dependencies.cmake para los detalles.
include(cmake/Dependencies.cmake)

# --- Opciones de compilación para NUESTRO código -----------------------------
# Las guardamos en una "interface library": un objetivo sin código propio que
# solo transporta opciones. Nuestros dos programas lo enlazan y heredan estas
# opciones. Las dependencias externas NO lo enlazan (sus advertencias no son
# problema nuestro).
add_library(luma_options INTERFACE)
target_compile_options(luma_options INTERFACE
    /W4            # Nivel alto de advertencias: el compilador avisa de errores probables.
    /permissive-   # Seguir el estándar de C++ estrictamente.
    /utf-8         # Los archivos fuente están en UTF-8 (tildes, ñ, etc.).
    /MP            # Compilar varios archivos en paralelo (más rápido).
)
target_compile_definitions(luma_options INTERFACE
    WIN32_LEAN_AND_MEAN  # windows.h incluye menos cosas: compila más rápido.
    NOMINMAX             # Evita que windows.h defina min/max y rompa código C++.
    UNICODE _UNICODE     # Usar texto Unicode en las funciones de Windows.
)

# --- Subproyectos -------------------------------------------------------------
add_subdirectory(client)    # -> Luma.dll
add_subdirectory(launcher)  # -> LumaLauncher.exe
