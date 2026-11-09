// =============================================================================
//  theme.h - Tema visual de Luma (configurable)
// =============================================================================
//  Identidad de Luma (del logo): carbón #25282B sobre marfil cálido #E9E6DF.
//
//  Estilo "liquid glass": paneles de vidrio que dejan ver el juego desenfocado
//  detrás, con un tinte, un brillo arriba y un borde que capta la luz. Hay dos
//  variantes:
//    - Oscuro: vidrio ahumado, texto marfil, acento marfil.
//    - Claro:  vidrio lechoso, texto carbón, acento carbón.
//
//  HILOS: con luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace luma::theme {

enum class Style { Dark = 0, Light = 1 };

struct Settings {
    Style style = Style::Dark;
    // Acento: por defecto ("automático") es el color de Luma que contrasta con
    // el estilo (marfil en oscuro, carbón en claro). Se puede elegir otro.
    bool accentAuto = true;
    float accent[3] = {0.914f, 0.902f, 0.875f};  // #E9E6DF
    float glassOpacity = 0.55f;   // cuánto tinte tiene el vidrio (0 = transparente)
    float blur = 0.75f;           // cuánto se desenfoca el juego detrás (0..1)
    float rounding = 18.0f;       // esquinas, en píxeles a 1080p
    float fontSize = 18.0f;       // en píxeles a 1080p (se escala con la resolución)
    bool animations = true;
    bool sounds = true;           // efectos de sonido del menú
    float soundVolume = 0.6f;     // 0..1
    float backgroundDim = 0.25f;  // cuánto se oscurece el juego detrás del menú (0..0.8)
};

Settings& Get();

// --- Colores de Luma ------------------------------------------------------------
inline constexpr ImVec4 kCharcoal{0.145f, 0.157f, 0.169f, 1.0f};  // #25282B
inline constexpr ImVec4 kIvory{0.914f, 0.902f, 0.875f, 1.0f};     // #E9E6DF

// Paleta del estilo actual (cambia con Settings::style).
struct Palette {
    ImVec4 glassTint;    // tinte del vidrio de los paneles grandes
    ImVec4 surface;      // superficies dentro del vidrio (tarjetas)
    ImVec4 surfaceHover;
    ImVec4 field;        // cajas de texto, botones
    ImVec4 text;
    ImVec4 textDim;
    ImVec4 border;       // borde del vidrio (la luz que capta)
    ImVec4 highlight;    // brillo especular arriba del vidrio
    ImVec4 shadow;
    ImVec4 trackOff;     // interruptor apagado
    ImVec4 knob;         // perilla del interruptor
    ImVec4 onAccentText; // texto sobre el color de acento
};
const Palette& P();

// Las fuentes del menú: texto normal, nombres de módulos y títulos. Las carga
// el overlay (overlay.cpp, LoadFonts).
struct Fonts {
    ImFont* regular = nullptr;  // texto general
    ImFont* medium = nullptr;   // nombres de módulos (más grande)
    ImFont* title = nullptr;    // títulos y marca (seminegrita)
};
Fonts& GetFonts();

// Escala de la interfaz según la resolución (1.0 = 1080p). La fija el overlay.
void SetUiScale(float scale);
float UiScale();

// Aplica el tema al estilo de ImGui (necesita un contexto de ImGui).
void Apply();

// El tamaño de letra cambió: el overlay tiene que regenerar la textura de la
// fuente (no se puede hacer en medio de un frame).
void RequestFontRebuild();
bool ConsumeFontRebuildRequest();

// El color de acento (el elegido, o el automático del estilo).
ImVec4 Accent(float alpha);
ImU32 AccentU32(float alpha);

// Un color con la transparencia multiplicada (útil para animaciones).
inline ImVec4 WithAlpha(ImVec4 c, float alphaMultiplier) {
    c.w *= alphaMultiplier;
    return c;
}

void Save(nlohmann::json& out);
void Load(const nlohmann::json& in);

}  // namespace luma::theme
