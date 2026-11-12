// =============================================================================
//  hud.h - Estilo y dibujo de los elementos del HUD (lo que se ve mientras juegas)
// =============================================================================
//  El menú de Luma es de "vidrio", pero el HUD tiene que encajar con el
//  juego: cajas cuadradas oscuras y translúcidas, texto nítido con sombra,
//  todo compacto (la idea es la de clients como Lunar). No copia nada del
//  juego: lo único que puede usar es la fuente que trae TU instalación de
//  Minecraft (ver "Fuentes"), y si no la encuentra usa la de Luma.
//
//  "PÍXELES DE HUD": igual que el juego, medimos el HUD en una cuadrícula de
//  píxeles grandes. Un píxel de HUD = varios píxeles de pantalla (2 a 1080p
//  con tamaño 1x). Así todo queda alineado y la letra pixelada se ve nítida.
//
//  Fuentes:
//    - "Pixel": la fuente pixelada del juego (Content\data\fonts\Mojangles.ttf),
//      leída de tu instalación. Luma NO la incluye ni la distribuye.
//      Para que se vea nítida, solo se dibuja a tamaños EXACTOS (múltiplos de
//      su cuadrícula): por eso el tamaño de los elementos va de a saltos.
//    - "Luma": Segoe UI Semibold de Windows, suave, a cualquier tamaño.
//
//  RENDIMIENTO: todo se dibuja directo en la "draw list" de ImGui (sin
//  ventanas ni layout) y sin pedir memoria.
//
//  HILOS: con luma::StateMutex() tomado (hilo de render o menú).
// =============================================================================
#pragma once

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace luma::render::hud {

enum class FontChoice { Pixel = 0, Luma = 1 };

// Estilo común de TODOS los elementos del HUD (se elige en Tema -> HUD).
struct Style {
    FontChoice font = FontChoice::Pixel;
    float background = 0.5f;    // opacidad de las cajas (0 = sin caja)
    float backgroundColor[3] = {0.0f, 0.0f, 0.0f};  // color de las cajas
    bool textShadow = true;     // sombra del texto, como en el juego
    float rounding = 0.0f;      // esquinas de las cajas, en píxeles de HUD
    bool border = false;        // borde fino alrededor de las cajas
    float text[3] = {1.0f, 1.0f, 1.0f};         // color del texto
    float pressed[3] = {1.0f, 1.0f, 1.0f};      // color de una tecla apretada (keystrokes)
    bool rainbow = false;       // el texto (y lo apretado) va cambiando de color (render/rainbow.h)
    float rainbowSpeed = 0.25f; // vueltas al arcoíris por segundo
    // Esconder el HUD en los menús del juego (pausa, inventario, chat...),
    // para que no quede encima de ellos. Luma lo sabe porque el juego
    // "suelta" el mouse al abrir un menú (sdk/mouse_grab.h).
    bool hideInGameMenus = true;
};
// El estilo GLOBAL (Tema -> HUD en pantalla).
Style& GetStyle();

// ESTILO PROPIO POR ELEMENTO: un módulo puede tener su propio estilo (por
// ejemplo, el FPS en verde y sin caja). El gestor de módulos llama a
// UseStyle(&suEstilo) antes de dibujarlo y UseStyle(nullptr) después; todas
// las funciones de dibujo de aquí usan el estilo "actual".
void UseStyle(const Style* style);
const Style& CurrentStyle();

// Guardar / cargar un estilo (el global o el de un elemento).
void SaveStyle(const Style& style, nlohmann::json& out);
void LoadStyle(const nlohmann::json& in, Style& style);

// Lo necesario para dibujar a una escala: qué fuente, de qué tamaño y
// cuántos píxeles de pantalla mide un píxel de HUD.
struct Metrics {
    ImFont* font = nullptr;
    float fontSize = 0.0f;
    float px = 1.0f;          // píxeles de pantalla por píxel de HUD
    float capTop = 0.0f;      // desde arriba del texto hasta arriba de las mayúsculas
    float capHeight = 0.0f;   // alto de las mayúsculas (lo que "se ve")
};

// Métricas para un elemento con escala "scale" (1 = normal).
Metrics Measure(float scale);

// Tamaño en pantalla de un texto.
ImVec2 TextSize(const Metrics& m, const char* text);

// Colores del estilo.
ImU32 TextColor(float alpha = 1.0f);
ImU32 PressedColor(float alpha = 1.0f);

// Texto con la sombra del juego (desplazada un píxel de HUD).
void Text(ImDrawList* draw, const Metrics& m, ImVec2 pos, const char* text, ImU32 color);

// While one of these lives, HUD text ignores the GENERAL efficiency mode
// (Luma Music uses it: the general mode doesn't touch the music).
struct IgnoreEfficiency {
    IgnoreEfficiency();
    ~IgnoreEfficiency();
};

// Caja de fondo. "pressed" (0..1) la ilumina, para las teclas apretadas.
void Box(ImDrawList* draw, const Metrics& m, ImVec2 min, ImVec2 max, float pressed = 0.0f);

// Texto centrado dentro de un rectángulo (centra las mayúsculas, no la línea).
void CenteredText(ImDrawList* draw, const Metrics& m, ImVec2 min, ImVec2 max, const char* text, ImU32 color);

// El elemento típico: una caja con un texto centrado. "minWidth" en píxeles
// de HUD (así varias cajas quedan del mismo ancho). Devuelve su tamaño.
ImVec2 TextBox(ImDrawList* draw, const Metrics& m, ImVec2 pos, const char* text, float minWidth = 0.0f);

// Alto estándar de una caja de una línea, en píxeles de pantalla.
float LineBoxHeight(const Metrics& m);

// --- Fuentes (las carga el overlay junto con las del menú) ---------------------

// Agrega las fuentes del HUD al atlas de ImGui. "uiScale" = escala por
// resolución (1 = 1080p).
void LoadFonts(ImGuiIO& io, float uiScale);
// ¿Se encontró la fuente pixelada del juego?
bool PixelFontAvailable();
// The game's pixel font at its native size (11 units), or nullptr.
ImFont* PixelFont();
// The smooth font (Segoe UI Semibold, loaded big: it looks good scaled down),
// or ImGui's if Windows doesn't have it.
ImFont* SmoothFont();
// The game's pixel font at exactly "k" screen pixels per font pixel (1..8),
// so every letter pixel is a crisp square; its size is 11 * k. nullptr if the
// game's font wasn't found.
ImFont* PixelFontAt(int k);

// F1: the game hides its HUD with F1 (for clean screenshots). Luma follows:
// each F1 while playing flips this, and the HUD modules aren't drawn while
// it's on. (The lyrics and the visualizer in the sky stay: they're part of
// the world, like the clouds.)
void ToggleF1Hidden();
bool F1Hidden();
// Once per frame: watches whether the GAME is drawing its HUD (sdk/game_crosshair.h)
// so Luma's HUD hides exactly when the game's does, even if F1 was pressed
// before Luma loaded. "playing" = in the world with the mouse grabbed.
void UpdateGameHudState(bool playing, bool firstPerson);

void Save(nlohmann::json& out);
void Load(const nlohmann::json& in);

}  // namespace luma::render::hud
