# =============================================================================
#  client/ -> Luma.dll
# =============================================================================
#  Una DLL (Dynamic Link Library) es un archivo con código que otro programa
#  puede cargar mientras se ejecuta. El launcher hará que Minecraft cargue
#  Luma.dll, y desde ahí nuestro código corre dentro del juego.
# =============================================================================

# SHARED = biblioteca dinámica (.dll). El nombre del objetivo es "Luma",
# así el archivo resultante se llama Luma.dll.
add_library(Luma SHARED
    core/dllmain.cpp
)

# Permite escribir #include "core/version.h" desde cualquier archivo de client/.
target_include_directories(Luma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Luma PRIVATE
    luma_options              # nuestras opciones de compilación (ver CMakeLists raíz)
    minhook                   # hooks (se usa desde la Fase 1/2)
    imgui                     # interfaz (se usa desde la Fase 2)
    nlohmann_json::nlohmann_json  # configuración JSON (se usa desde la Fase 3)
)
