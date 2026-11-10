// =============================================================================
//  draw3d.h - Dibujar DENTRO del mundo, con las herramientas del juego (Fase 8)
// =============================================================================
//  El overlay de Luma dibuja ENCIMA de la imagen: todo se ve a través de las
//  paredes. Para el contorno de bloque y los hitboxes queremos lo contrario:
//  líneas que estén en el mundo y que las paredes tapen. Para eso usamos las
//  mismas piezas con las que el juego dibuja el contorno del bloque que miras
//  (la idea de cuáles son viene del client open source Latite; el código es
//  propio):
//
//    - Tessellator: el "armador de mallas" del juego. Se le dice "empieza
//      líneas", "color", "vértice", "vértice"...  (Tessellator::begin/vertex)
//    - MeshHelpers::renderMeshImmediately: dibuja lo armado con un material.
//    - Material "selection_box": el del contorno del juego (respeta la
//      profundidad: lo que está detrás de una pared no se ve).
//
//  Las coordenadas se pasan RELATIVAS a la cámara (LevelRendererPlayer +0x658).
//  Todo pasa dentro de LevelRenderer::renderLevel (sdk/camera.cpp lo llama).
//
//  Si alguna firma no se encuentra, Available() es false y los módulos que
//  dibujan en el mundo quedan "no disponibles".
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::draw3d {

bool Init();
bool Available();

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};

// --- Lo que se dibuja ---------------------------------------------------------------
// Contorno de bloque personalizado: reemplaza al del juego (renderOutlineSelection).
struct OutlineSettings {
    bool enabled = false;
    Color line{1.0f, 1.0f, 1.0f, 1.0f};
    bool fill = false;
    Color fillColor{1.0f, 1.0f, 1.0f, 0.15f};
    int fillMaterial = 0;  // cuál de los materiales candidatos usar (0..kFillMaterialCount-1)
};
inline constexpr int kFillMaterialCount = 4;
void SetOutline(const OutlineSettings& settings);

// Hitboxes: la caja de cada entidad cercana (y opcionalmente hacia dónde mira).
struct HitboxSettings {
    bool enabled = false;
    bool players = true;        // otros jugadores
    Color playerColor{1.0f, 1.0f, 1.0f, 1.0f};
    bool mobs = true;           // animales, monstruos y demás entidades
    Color mobColor{1.0f, 1.0f, 1.0f, 1.0f};
    bool items = false;         // ítems tirados, orbes de experiencia, flechas...
    Color itemColor{1.0f, 1.0f, 1.0f, 1.0f};
    float maxDistance = 32.0f;  // bloques
    bool showSelf = false;      // tu propia caja (solo se ve en tercera persona)
};
void SetHitboxes(const HitboxSettings& settings);

// sdk/camera.cpp las llama alrededor de LevelRenderer::renderLevel.
void BeginLevel(std::uintptr_t levelRenderer);
void EndLevel();

// Llamado por sdk/camera.cpp justo después de que el juego dibuja el mundo.
void OnRenderLevel(std::uintptr_t levelRenderer, std::uintptr_t screenContext);

}  // namespace luma::sdk::draw3d
