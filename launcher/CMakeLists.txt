# =============================================================================
#  launcher/ -> LumaLauncher.exe (ventana) y LumaLauncherCli.exe (consola)
# =============================================================================
#  launcher_core: lo que no es interfaz (buscar el juego, inyectar, el flujo
#  de JUGAR). Lo usan los dos programas:
#    - LumaLauncher.exe: la ventana con diseño (Dear ImGui + Direct3D 11).
#    - LumaLauncherCli.exe: la versión de consola de siempre, para pruebas y
#      scripts (--no-pause, --skip-gdk-check).
# =============================================================================

add_library(launcher_core STATIC
    src/injector.cpp
    src/game_process.cpp
    src/flow.cpp
    ${PROJECT_SOURCE_DIR}/shared/i18n.cpp
)
target_include_directories(launcher_core PUBLIC
    src
    ${PROJECT_SOURCE_DIR}/shared
)
target_link_libraries(launcher_core PUBLIC
    luma_options
    shell32   # SHGetKnownFolderPath, ShellExecuteW
    ole32     # CoTaskMemFree
)

# --- La ventana -----------------------------------------------------------------
add_executable(LumaLauncher WIN32
    src/gui/window.cpp
    src/gui/app.cpp
    src/gui/theme.cpp
    ${PROJECT_SOURCE_DIR}/client/render/icons.cpp   # el mismo dibujador de íconos del menú
    assets/launcher.rc
)
target_include_directories(LumaLauncher PRIVATE
    src
    ${PROJECT_SOURCE_DIR}/client   # render/icons.h, core/version.h
)
# Luma.dll va DENTRO de LumaLauncher.exe (recurso 2, datos crudos): así el
# launcher es un solo archivo que se puede copiar a cualquier lado. Generamos
# un .rc que apunta a la DLL recién compilada (Debug o Release, la que
# corresponda) y hacemos que el launcher se compile después de la DLL.
file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/luma_dll.rc
    CONTENT "2 RCDATA \"$<TARGET_FILE:Luma>\"
"
)
target_sources(LumaLauncher PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/luma_dll.rc)
add_dependencies(LumaLauncher Luma)

target_link_libraries(LumaLauncher PRIVATE
    launcher_core
    imgui
    nlohmann_json::nlohmann_json
    dwmapi    # sombra y esquinas redondeadas de Windows 11
)

# --- La consola (pruebas) ----------------------------------------------------------
add_executable(LumaLauncherCli
    src/cli_main.cpp
)
target_link_libraries(LumaLauncherCli PRIVATE launcher_core)
