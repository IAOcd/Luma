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
    src/gui/sky.cpp      # el cielo pixelado del cartel
    src/gui/textures.cpp # imágenes (noticias, skins) como texturas
    src/gui/skin3d.cpp   # tu skin en 3D
    src/gui/skins.cpp    # la biblioteca de skins
    ${PROJECT_SOURCE_DIR}/shared/skin_pack.cpp  # la biblioteca como paquete de skins del juego
    src/account.cpp      # iniciar sesión con Microsoft/Xbox (opcional)
    src/net.cpp          # pedidos HTTPS (WinHTTP)
    src/signature.cpp    # comprobar la firma de las actualizaciones
    src/updater.cpp      # actualizaciones automáticas desde GitHub
    ${PROJECT_SOURCE_DIR}/client/render/icons.cpp   # el mismo dibujador de íconos del menú
    ${PROJECT_SOURCE_DIR}/client/render/nanosvg_impl.cpp
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

# Compilación OFICIAL (la hace GitHub Actions con -DLUMA_OFFICIAL_BUILD=ON).
# Solo esas se reemplazan solas con las actualizaciones: tus compilaciones
# locales nunca se pisan.
option(LUMA_OFFICIAL_BUILD "Compilación oficial (se actualiza sola)" OFF)
if(LUMA_OFFICIAL_BUILD)
    target_compile_definitions(LumaLauncher PRIVATE LUMA_OFFICIAL_BUILD)
endif()

target_link_libraries(LumaLauncher PRIVATE
    launcher_core
    imgui
    nanosvg
    nlohmann_json::nlohmann_json
    dwmapi         # sombra y esquinas redondeadas de Windows 11
    winhttp        # HTTPS con los servidores de Microsoft y Xbox
    ws2_32         # el mini servidor en localhost que recibe el código de inicio de sesión
    bcrypt         # números aleatorios y SHA-256 (PKCE)
    crypt32        # DPAPI: guardar la sesión cifrada
    windowscodecs  # WIC: decodificar la foto de perfil
    shlwapi        # SHCreateMemStream
)

# --- La consola (pruebas) ----------------------------------------------------------
add_executable(LumaLauncherCli
    src/cli_main.cpp
)
target_link_libraries(LumaLauncherCli PRIVATE launcher_core)

# nanosvg es código ajeno: sin avisos del compilador sobre él.
set_source_files_properties(${PROJECT_SOURCE_DIR}/client/render/nanosvg_impl.cpp
    TARGET_DIRECTORY LumaLauncher PROPERTIES COMPILE_OPTIONS "/W0")
