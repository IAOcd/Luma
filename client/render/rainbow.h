// =============================================================================
//  rainbow.h - El color "rainbow" (arcoíris que va cambiando)
// =============================================================================
//  Una sola fuente de color para todo Luma (HUD, mira, contorno, hitboxes,
//  bordes de chunk): así todo cambia de color sincronizado.
//
//  El color sale del "tono" (hue), que da una vuelta completa al arcoíris cada
//  1/velocidad segundos. "offset" (0..1) corre el tono: sirve para que cada
//  píxel de la mira tenga un color distinto (efecto de onda).
//
//  Es barato: unas cuentas por llamada, sin memoria. Se puede usar desde
//  cualquier hilo (no depende de ImGui).
// =============================================================================
#pragma once

#include <windows.h>

#include <cmath>

namespace luma::rainbow {

// Segundos desde que arrancó la PC (con decimales, muy preciso).
inline double Seconds() {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// Tono (0..1), saturación y brillo -> rojo, verde y azul (0..1).
inline void Hsv(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    const float x = h * 6.0f;
    const int i = static_cast<int>(x) % 6;
    const float f = x - std::floor(x);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// El color del arcoíris ahora. "speed" = vueltas por segundo.
inline void Color(float speed, float offset, float& r, float& g, float& b) {
    const double turns = Seconds() * static_cast<double>(speed) + static_cast<double>(offset);
    Hsv(static_cast<float>(turns - std::floor(turns)), 0.7f, 1.0f, r, g, b);
}

inline constexpr float kDefaultSpeed = 0.25f;  // una vuelta cada 4 segundos

}  // namespace luma::rainbow
