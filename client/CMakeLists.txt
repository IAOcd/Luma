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
    core/clicks.cpp
    core/config.cpp
    core/dllmain.cpp
    core/hooks.cpp
    core/keys.cpp
    core/logger.cpp
    core/luma.cpp
    core/paths.cpp
    modules/module_manager.cpp
    modules/registry.cpp
    modules/hud/hud_modules.cpp
    modules/hud/inventory_hud.cpp
    modules/hud/player_hud.cpp
    modules/hud/watermark.cpp
    modules/performance/perf_overlay.cpp
    modules/performance/process_opts.cpp
    modules/utility/mouse_tweaks.cpp
    modules/utility/toggle_keys.cpp
    modules/visual/camera_mods.cpp
    modules/visual/crosshair.cpp
    modules/visual/post_processing.cpp
    modules/visual/world_draw.cpp
    perf/benchmark.cpp
    perf/frame_stats.cpp
    perf/windows_info.cpp
    render/assets.cpp
    render/cinematic.cpp
    render/dx12_hooks.cpp
    render/glass.cpp
    render/gpu.cpp
    render/hud.cpp
    render/hud_editor.cpp
    render/icons.cpp
    render/input.cpp
    render/item_icons.cpp
    render/loading.cpp
    render/menu.cpp
    render/module_options.cpp
    render/notifications.cpp
    render/overlay.cpp
    render/postfx.cpp
    render/sound.cpp
    render/theme.cpp
    sdk/mouse_grab.cpp
    sdk/camera.cpp
    sdk/draw3d.cpp
    sdk/entity.cpp
    sdk/inventory.cpp
    sdk/memory.cpp
    sdk/mouse_queue.cpp
    sdk/network.cpp
    sdk/player.cpp
    sdk/scanner.cpp
    # Recursos incrustados en la DLL (el logo). Visual Studio compila los .rc solo.
    assets/luma.rc
)

# Permite escribir #include "core/version.h" desde cualquier archivo de client/.
target_include_directories(Luma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Luma PRIVATE
    luma_options              # nuestras opciones de compilación (ver CMakeLists raíz)
    minhook                   # hooks (se usa desde la Fase 1/2)
    imgui                     # interfaz (se usa desde la Fase 2)
    nlohmann_json::nlohmann_json  # configuración JSON (se usa desde la Fase 3)
    shell32                   # SHGetKnownFolderPath (carpeta %APPDATA%)
    ole32                     # CoTaskMemFree y COM (lo usan WIC y XAudio2)
    xaudio2                   # efectos de sonido del menú (viene con Windows 10/11)
    windowscodecs             # WIC: decodificar el PNG del logo
    d3d12                     # el desenfoque del vidrio (render/gpu.cpp)
    d3dcompiler               # compilar el shader del desenfoque
    winmm                     # timeBeginPeriod (optimización "Timer de 1 ms")
    powrprof                  # leer el plan de energía (consejos de Windows)
)
