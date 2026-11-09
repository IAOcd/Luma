# =============================================================================
#  Dependencias externas (FetchContent)
# =============================================================================
#  FetchContent es una función de CMake que DESCARGA código de terceros la
#  primera vez que configuras el proyecto y lo compila junto con el nuestro.
#  Así el repositorio no guarda archivos ajenos: solo esta lista.
#
#  Cada dependencia tiene la versión FIJA (un tag de release exacto). Nunca
#  usamos "la última", porque una actualización inesperada podría romper la
#  compilación de un día para otro.
#
#  Para actualizar una dependencia: cambia la URL a la nueva versión, borra la
#  carpeta build/ y vuelve a configurar.
# =============================================================================

include(FetchContent)

# Descargar en silencio salvo errores, y no volver a descargar si ya existe.
set(FETCHCONTENT_QUIET ON)

# --- MinHook ------------------------------------------------------------------
# Biblioteca para "hookear" funciones: interceptar una función del juego (por
# ejemplo, la que dibuja cada frame) para ejecutar nuestro código antes o
# después. Licencia BSD-2: podemos usarla libremente citando el aviso.
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# --- nlohmann/json -------------------------------------------------------------
# Biblioteca para leer y escribir archivos JSON (la configuración de Luma).
# Licencia MIT. Usamos el paquete reducido "json.tar.xz" que publica el autor,
# que es mucho más liviano que todo el repositorio.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# --- Dear ImGui ----------------------------------------------------------------
# Biblioteca para dibujar interfaces (menús, botones, ventanas) encima del
# juego. Licencia MIT. ImGui no trae CMakeLists propio, así que más abajo
# creamos nosotros la biblioteca a partir de sus archivos.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Descargar todo (y, para minhook y json, procesar su CMakeLists).
FetchContent_MakeAvailable(minhook nlohmann_json imgui)

# --- Biblioteca ImGui (solo el núcleo) ------------------------------------------
# Por ahora solo el núcleo de ImGui. Los "backends" (el código que conecta
# ImGui con DirectX y con la ventana de Windows) se agregan en la Fase 2,
# cuando decidamos qué API gráfica usa el juego.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
