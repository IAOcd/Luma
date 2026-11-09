# =============================================================================
#  launcher/ -> LumaLauncher.exe
# =============================================================================
#  Programa de consola que busca Minecraft.Windows.exe y le inyecta Luma.dll.
# =============================================================================

add_executable(LumaLauncher
    src/main.cpp
    src/injector.cpp
    src/process.cpp
)

target_link_libraries(LumaLauncher PRIVATE
    luma_options
    shell32   # SHGetKnownFolderPath (carpeta %APPDATA%)
    ole32     # CoTaskMemFree
)
