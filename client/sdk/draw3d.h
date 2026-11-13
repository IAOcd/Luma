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
#include <string>
#include <vector>

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
    bool rainbow = false;  // la línea (y el relleno) van cambiando de color
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
    bool rainbow = false;       // todas las cajas van cambiando de color
};
void SetHitboxes(const HitboxSettings& settings);

// Bordes de chunk (como el F3+G de Java).
struct ChunkBorderSettings {
    bool enabled = false;
    Color inner{1.0f, 0.85f, 0.25f, 0.9f};    // paredes del chunk actual (cada 2 bloques)
    Color corner{0.35f, 0.65f, 1.0f, 1.0f};   // sus esquinas
    Color neighbor{1.0f, 0.35f, 0.3f, 0.9f};  // esquinas de los chunks vecinos
    bool neighbors = true;
    int range = 24;                            // bloques arriba y abajo de ti
    bool rainbow = false;                      // las líneas van cambiando de color
};
void SetChunkBorders(const ChunkBorderSettings& settings);

// --- Filled shapes in the world (Lyric Flow's letters) ---------------------------
// Triangles in WORLD coordinates (x, y, z per vertex, 3 vertices per
// triangle), one color per group. They're drawn by the game together with
// the rest of the world: the terrain in front covers them, like a block.
// Each group is one draw call. Replaced every frame (call it every frame
// while you want them; an empty list = nothing).
struct WorldShapes {
    Color color;
    std::vector<float> triangles;
    // true = "triangles" holds LINES instead (2 points each, 6 floats), drawn
    // with the game's own outline material: the one that is sure to show
    // anywhere (filled triangles don't show far away with these materials).
    bool lines = false;
};
// Each module uses its own "channel", so they don't erase each other's shapes.
enum ShapeChannel { kChannelLyrics = 0, kChannelVisualizer = 1, kChannelWaypoints = 2, kChannelCount = 3 };
void SetWorldShapes(std::vector<WorldShapes> shapes, int channel = kChannelLyrics);
// Which of the fill materials draws them (the same candidates as the outline fill).
void SetWorldShapesMaterial(int index);

// --- Con textura (las capas de Luma) ----------------------------------------------------
// Triángulos con una imagen encima. "texture" = RUTA COMPLETA de un PNG en el
// disco (el juego lo carga una vez y lo guarda). "vertices": por vértice
// x, y, z (del mundo), u, v (0..1 dentro de la imagen); 3 vértices por
// triángulo. Se ven de un solo lado: para verlos de los dos, mandar cada
// triángulo dos veces (al revés).
struct TexturedTriangles {
    std::string texture;
    std::vector<float> vertices;
};
// Quien arma lo que se dibuja con textura: se lo llama UNA vez por frame, en
// el hilo de render, justo antes de dibujar ("partialTick": cuánto avanzó el
// juego hacia el próximo tick, 0..1, para que lo que sigue a un jugador no se
// vea a saltos). Llena "out". nullptr = nada.
using TexturedProvider = void (*)(float partialTick, std::vector<TexturedTriangles>& out);
void SetTexturedProvider(TexturedProvider provider);
// Busca y engancha lo necesario para dibujar con textura (una vez; después
// de Init). Lo llama quien lo necesita (las capas).
bool InitTextures();
// ¿Se encontró cómo dibujar con textura en esta versión del juego?
bool TexturesAvailable();

// sdk/camera.cpp las llama alrededor de LevelRenderer::renderLevel.
void BeginLevel(std::uintptr_t levelRenderer);
void EndLevel();

// Llamado por sdk/camera.cpp justo después de que el juego dibuja el mundo.
void OnRenderLevel(std::uintptr_t levelRenderer, std::uintptr_t screenContext);

}  // namespace luma::sdk::draw3d
