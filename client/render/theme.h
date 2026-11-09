// =============================================================================
//  theme.h - Tema visual de Luma (configurable)
// =============================================================================
//  Color de acento, opacidad, esquinas redondeadas, tamaño de letra y
//  animaciones. Se guarda en el perfil y se aplica al estilo de ImGui.
//
//  HILOS: con luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace luma::theme {

struct Settings {
    float accent[3] = {1.00f, 0.72f, 0.30f};  // ámbar cálido (R, G, B de 0 a 1)
    float windowOpacity = 0.92f;
    float rounding = 12.0f;
    float fontSize = 18.0f;  // en píxeles a 1080p (se escala con la resolución)
    bool animations = true;
};

Settings& Get();

// Las fuentes del menú: texto normal, nombres de módulos y títulos. Las carga
// el overlay (overlay.cpp, LoadFonts). Cualquiera puede ser nullptr antes de
// cargarse; usar Font() que devuelve la normal en ese caso.
struct Fonts {
    ImFont* regular = nullptr;  // texto general
    ImFont* medium = nullptr;   // nombres de módulos (más grande)
    ImFont* title = nullptr;    // títulos y marca (aún más grande, seminegrita)
};
Fonts& GetFonts();

// Paleta del menú (colores fijos que combinan con cualquier acento).
namespace palette {
inline constexpr ImVec4 kPanel{0.135f, 0.150f, 0.190f, 1.0f};      // fondo de los paneles
inline constexpr ImVec4 kPanelDark{0.105f, 0.115f, 0.145f, 1.0f};  // barra lateral
inline constexpr ImVec4 kTile{0.205f, 0.225f, 0.280f, 1.0f};       // tarjetas
inline constexpr ImVec4 kTileHover{0.245f, 0.268f, 0.330f, 1.0f};
inline constexpr ImVec4 kField{0.300f, 0.320f, 0.380f, 1.0f};      // caja de búsqueda
inline constexpr ImVec4 kText{0.930f, 0.940f, 0.960f, 1.0f};
inline constexpr ImVec4 kTextDim{0.620f, 0.650f, 0.720f, 1.0f};
inline constexpr ImVec4 kOn{0.300f, 0.780f, 0.420f, 1.0f};         // interruptor encendido
inline constexpr ImVec4 kOff{0.840f, 0.290f, 0.330f, 1.0f};        // interruptor apagado
}  // namespace palette

// Escala de la interfaz según la resolución (1.0 = 1080p). La fija el overlay.
void SetUiScale(float scale);
float UiScale();

// Aplica el tema al estilo de ImGui (necesita un contexto de ImGui).
void Apply();

// El tamaño de letra cambió: el overlay tiene que regenerar la textura de la
// fuente (no se puede hacer en medio de un frame).
void RequestFontRebuild();
bool ConsumeFontRebuildRequest();

// El color de acento, en los dos formatos que usa ImGui.
ImVec4 Accent(float alpha);
ImU32 AccentU32(float alpha);

void Save(nlohmann::json& out);
void Load(const nlohmann::json& in);

}  // namespace luma::theme
