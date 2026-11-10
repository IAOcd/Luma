// =============================================================================
//  skin3d.h - Tu skin de Minecraft en 3D (dibujada con ImGui, sin shaders)
// =============================================================================
//  CÓMO SE DIBUJA UN MODELO 3D CON SOLO "CUADRADOS CON TEXTURA"
//    El personaje de Minecraft son cajas: cabeza, cuerpo, 2 brazos y 2 piernas
//    (más una "segunda capa" un poco más grande: sombrero, chaqueta, mangas,
//    pantalón). Cada caja tiene 6 caras, y cada cara usa un rectángulo de la
//    imagen de la skin (el "mapa de UV" estándar de 64x64 de Minecraft).
//
//    1. Se calculan las 4 esquinas de cada cara en 3D.
//    2. Se GIRAN (el ángulo que eliges arrastrando con el mouse, y una
//       animación suave de brazos y piernas).
//    3. Se PROYECTAN a 2D (lo lejano se ve un poco más chico: perspectiva).
//    4. Se descartan las caras que miran para atrás, y las demás se dibujan
//       de la más lejana a la más cercana ("algoritmo del pintor").
//    Son ~144 cuadrados por frame: nada para la GPU.
//
//  Los píxeles se dibujan SIN suavizar (filtro "punto"): por eso la skin se
//  ve nítida y pixelada aunque esté grande.
// =============================================================================
#pragma once

#include <imgui.h>

struct ID3D11Device;

namespace luma::launcher::gui::skin3d {

struct Model {
    ImTextureID texture{};
    bool slim = false;    // brazos finos (3 píxeles, modelo "Alex")
    bool legacy = false;  // skin vieja de 64x32 (sin segunda capa ni extremidades izquierdas propias)
};

struct View {
    float yaw = 0.0f;     // giro horizontal (radianes; 0 = de frente)
    float pitch = 0.12f;  // un poco desde arriba
    float zoom = 1.0f;    // 1 = cuerpo entero; más = acercar a la cabeza
    float time = 0.0f;    // para la animación
    float alpha = 1.0f;
};

void Init(ID3D11Device* device);  // crea el filtro "punto" para los píxeles nítidos
void Shutdown();

// Dibuja el personaje dentro de [min, max]. Devuelve el punto de la pantalla
// justo encima de la cabeza (para poner el nombre).
ImVec2 Draw(ImDrawList* d, ImVec2 min, ImVec2 max, const Model& model, const View& view);

// ¿La skin es de brazos finos? Mira si la columna exterior del brazo derecho
// está vacía (transparente), como en las skins "Alex". rgba = la imagen.
bool DetectSlim(const unsigned char* rgba, int width, int height);

}  // namespace luma::launcher::gui::skin3d
