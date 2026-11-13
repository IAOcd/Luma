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
    core/stats.cpp
    core/tr.cpp                             # traducciones de los textos del juego
    core/cloud.cpp                          # hablar con el servidor de Luma (usuarios de Luma)
    core/media.cpp                          # la música que suena en Windows (módulo de música)
    core/lyrics.cpp                         # las letras de la canción (LRCLIB)
    core/audio_viz.cpp                      # el sonido de la musica, para el visualizer
    core/text_bidi.cpp                      # arabe y hebreo: de derecha a izquierda
    core/spotify.cpp                        # conexión opcional con Spotify (la cola: "A continuación")
    ${PROJECT_SOURCE_DIR}/shared/skin_pack.cpp  # tus skins como paquete del juego
    ${PROJECT_SOURCE_DIR}/shared/i18n.cpp   # textos en inglés, español y portugués (los mismos del launcher)
    modules/module_manager.cpp
    modules/registry.cpp
    modules/hud/hud_modules.cpp
    modules/hud/inventory_hud.cpp
    modules/hud/paperdoll.cpp
    modules/hud/player_list.cpp
    modules/hud/music.cpp
    modules/hud/debug_screen.cpp
    modules/world/minimap.cpp
    modules/world/freecam.cpp
    modules/world/waypoints.cpp
    modules/world/map_store.cpp
    modules/world/map_look.cpp
    render/world_map.cpp
    render/pixel_art.cpp
    modules/hud/scoreboard.cpp
    modules/hud/player_hud.cpp
    modules/hud/watermark.cpp
    modules/performance/perf_overlay.cpp
    modules/performance/process_opts.cpp
    modules/utility/mouse_tweaks.cpp
    modules/utility/toggle_keys.cpp
    modules/visual/camera_mods.cpp
    modules/visual/crosshair.cpp
    modules/visual/dynamic_lights.cpp
    modules/visual/post_processing.cpp
    modules/visual/world_draw.cpp
    perf/benchmark.cpp
    perf/frame_stats.cpp
    perf/gpu_timer.cpp
    perf/system_info.cpp
    perf/windows_info.cpp
    render/assets.cpp
    render/cinematic.cpp
    render/dx12_hooks.cpp
    render/glass.cpp
    render/gpu.cpp
    render/hud.cpp
    render/hud_editor.cpp
    render/icons.cpp
    render/lyric_flow.cpp
    render/font_fallback.cpp
    render/visualizer.cpp
    render/nanosvg_impl.cpp
    render/input.cpp
    render/item_icons.cpp
    render/loading.cpp
    render/menu.cpp
    render/music.cpp
    render/module_options.cpp
    render/notifications.cpp
    render/overlay.cpp
    render/postfx.cpp
    render/skin_heads.cpp
    render/sound.cpp
    render/theme.cpp
    sdk/mouse_grab.cpp
    sdk/camera.cpp
    sdk/game_options.cpp
    sdk/draw3d.cpp
    sdk/blocks.cpp
    sdk/radar.cpp
    sdk/worldid.cpp
    sdk/entity.cpp
    sdk/inventory.cpp
    sdk/memory.cpp
    sdk/paperdoll.cpp
    sdk/hud_ui.cpp
    sdk/dynlight.cpp                        # dynamic lights (luz de la mano)
    sdk/game_crosshair.cpp                  # esconder la mira del juego
    sdk/skin_apply.cpp                      # Equipar cambia la skin al instante
    sdk/watch.cpp                           # diagnóstico: quién escribe en una dirección
    sdk/skin.cpp
    sdk/mouse_queue.cpp
    sdk/network.cpp
    sdk/player.cpp
    sdk/players.cpp
    sdk/scanner.cpp
    # Recursos incrustados en la DLL (el logo). Visual Studio compila los .rc solo.
    assets/luma.rc
)

# Permite escribir #include "core/version.h" desde cualquier archivo de client/.
target_include_directories(Luma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/shared)

target_link_libraries(Luma PRIVATE
    luma_options              # nuestras opciones de compilación (ver CMakeLists raíz)
    minhook                   # hooks (se usa desde la Fase 1/2)
    imgui                     # interfaz (se usa desde la Fase 2)
    nanosvg                   # dibujar los íconos SVG con bordes suaves
    nlohmann_json::nlohmann_json  # configuración JSON (se usa desde la Fase 3)
    shell32                   # SHGetKnownFolderPath (carpeta %APPDATA%)
    ole32                     # CoTaskMemFree y COM (lo usan WIC y XAudio2)
    xaudio2                   # efectos de sonido del menú (viene con Windows 10/11)
    windowscodecs             # WIC: decodificar el PNG del logo
    d3d12                     # el desenfoque del vidrio (render/gpu.cpp)
    dxgi                      # el nombre de la tarjeta gráfica (pantalla de depuración F3)
    version                   # la versión de Minecraft.Windows.exe (pantalla de depuración F3)
    d3dcompiler               # compilar el shader del desenfoque
    winmm                     # timeBeginPeriod (optimización "Timer de 1 ms")
    powrprof                  # leer el plan de energía (consejos de Windows)
    winhttp                   # HTTPS al servidor de Luma (core/cloud.cpp)
    windowsapp                # APIs de Windows "WinRT": la música que suena (core/media.cpp)
    crypt32                   # DPAPI: abrir el pase de Luma cifrado
    ws2_32                    # Spotify: el mini servidor en 127.0.0.1 para volver del navegador
    bcrypt                    # Spotify: números aleatorios y SHA-256 (PKCE)
    mmdevapi                  # el visualizer: escuchar el sonido de la PC (ActivateAudioInterfaceAsync)
)

# nanosvg es código ajeno: sin avisos del compilador sobre él (por ejemplo
# C4702 "código inalcanzable", que con /WX cortaría la compilación).
set_source_files_properties(render/nanosvg_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
