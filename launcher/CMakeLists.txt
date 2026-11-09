# =============================================================================
#  launcher/ -> LumaLauncher.exe
# =============================================================================
#  Programa que buscará Minecraft.Windows.exe y le inyectará Luma.dll.
#  En la Fase 0 solo muestra un mensaje.
# =============================================================================

add_executable(LumaLauncher
    src/main.cpp
)

target_link_libraries(LumaLauncher PRIVATE luma_options)
