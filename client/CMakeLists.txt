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
    core/config.cpp
    core/dllmain.cpp
    core/hooks.cpp
    core/keys.cpp
    core/logger.cpp
    core/luma.cpp
    core/paths.cpp
    modules/module_manager.cpp
    modules/registry.cpp
    modules/hud/watermark.cpp
    modules/performance/perf_overlay.cpp
    perf/frame_stats.cpp
    render/dx12_hooks.cpp
    render/input.cpp
    render/loading.cpp
    render/menu.cpp
    render/notifications.cpp
    render/overlay.cpp
    render/sound.cpp
    render/theme.cpp
    sdk/mouse_grab.cpp
    sdk/mouse_queue.cpp
    sdk/scanner.cpp
)

# Permite escribir #include "core/version.h" desde cualquier archivo de client/.
target_include_directories(Luma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Luma PRIVATE
    luma_options              # nuestras opciones de compilación (ver CMakeLists raíz)
    minhook                   # hooks (se usa desde la Fase 1/2)
    imgui                     # interfaz (se usa desde la Fase 2)
    nlohmann_json::nlohmann_json  # configuración JSON (se usa desde la Fase 3)
    shell32                   # SHGetKnownFolderPath (carpeta %APPDATA%)
    ole32                     # CoTaskMemFree (liberar el texto que devuelve la anterior)
    winmm                     # PlaySound (efectos de sonido del menú)
)
