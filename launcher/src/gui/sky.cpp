// =============================================================================
//  sky.cpp - El cielo pixelado (ver sky.h)
// =============================================================================
//  CÓMO FUNCIONA
//    1. Según la hora (o el modo fijo de Ajustes) se mezclan "fotos" de colores
//       del cielo: noche, amanecer, día, atardecer, anochecer. Entre una y otra
//       los colores se mezclan de a poco (sin saltos).
//    2. Se dibuja en una "cuadrícula" de píxeles grandes: todo cae justo en la
//       cuadrícula, así se ve pixelado como un juego de bloques.
//    3. Estrellas, nubes, sol/luna y suelo usan números pseudoaleatorios FIJOS
//       (una fórmula a partir de un número), así no "saltan" entre frames.
// =============================================================================
#include "gui/sky.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace luma::launcher::gui::sky {

namespace {

constexpr float kPi = 3.14159265f;

struct Rgb {
    float r, g, b;
};

constexpr Rgb Hex(unsigned v) {
    return {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f};
}

Rgb Lerp(Rgb a, Rgb b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

ImU32 U32(Rgb c, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, alpha));
}

// Los colores de una hora del día.
struct Palette {
    float hour;
    Rgb top, middle, horizon;  // degradado del cielo
    Rgb cloud, cloudShade;     // nubes (cara clara y borde de abajo)
    Rgb ground, groundTop;     // suelo y su borde
    float stars;               // 0..1: cuánto se ven las estrellas
};

// Ordenadas por hora; la última vuelve a la primera (medianoche).
const Palette kPalettes[] = {
    {0.0f, Hex(0x0B1030), Hex(0x18205A), Hex(0x2B3170), Hex(0x2E3768), Hex(0x232A55), Hex(0x0A0D22), Hex(0x1A2148), 1.0f},
    {5.0f, Hex(0x0B1030), Hex(0x18205A), Hex(0x2B3170), Hex(0x2E3768), Hex(0x232A55), Hex(0x0A0D22), Hex(0x1A2148), 1.0f},
    {6.3f, Hex(0x2A3B7A), Hex(0x7C6FB0), Hex(0xF2A97E), Hex(0xF5C1B5), Hex(0xD99C93), Hex(0x1C1F3D), Hex(0x3A3368), 0.15f},
    {8.0f, Hex(0x3E7BD6), Hex(0x6FA6EA), Hex(0xBFDCF5), Hex(0xFFFFFF), Hex(0xD6E3F3), Hex(0x24365E), Hex(0x3C5A8C), 0.0f},
    {16.5f, Hex(0x3E7BD6), Hex(0x6FA6EA), Hex(0xBFDCF5), Hex(0xFFFFFF), Hex(0xD6E3F3), Hex(0x24365E), Hex(0x3C5A8C), 0.0f},
    {18.0f, Hex(0x2C4A9A), Hex(0x5B5DB4), Hex(0xF09A7A), Hex(0xF6C4B8), Hex(0xDFA196), Hex(0x161B3D), Hex(0x3A3F86), 0.25f},
    {19.6f, Hex(0x141C4E), Hex(0x2E3484), Hex(0x7B5AA6), Hex(0x6A5C9E), Hex(0x554A85), Hex(0x0E1230), Hex(0x272C66), 0.8f},
    {21.0f, Hex(0x0B1030), Hex(0x18205A), Hex(0x2B3170), Hex(0x2E3768), Hex(0x232A55), Hex(0x0A0D22), Hex(0x1A2148), 1.0f},
    {24.0f, Hex(0x0B1030), Hex(0x18205A), Hex(0x2B3170), Hex(0x2E3768), Hex(0x232A55), Hex(0x0A0D22), Hex(0x1A2148), 1.0f},
};

Palette PaletteAt(float hour) {
    for (std::size_t i = 0; i + 1 < std::size(kPalettes); ++i) {
        const Palette& a = kPalettes[i];
        const Palette& b = kPalettes[i + 1];
        if (hour >= a.hour && hour <= b.hour) {
            float t = (hour - a.hour) / std::max(0.001f, b.hour - a.hour);
            t = t * t * (3.0f - 2.0f * t);
            return {hour,
                    Lerp(a.top, b.top, t),
                    Lerp(a.middle, b.middle, t),
                    Lerp(a.horizon, b.horizon, t),
                    Lerp(a.cloud, b.cloud, t),
                    Lerp(a.cloudShade, b.cloudShade, t),
                    Lerp(a.ground, b.ground, t),
                    Lerp(a.groundTop, b.groundTop, t),
                    a.stars + (b.stars - a.stars) * t};
        }
    }
    return kPalettes[0];
}

// Hora actual (con fracción): 18.5 = 18:30.
float CurrentHour() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return static_cast<float>(t.wHour) + static_cast<float>(t.wMinute) / 60.0f + static_cast<float>(t.wSecond) / 3600.0f;
}

// Número pseudoaleatorio fijo en [0, 1) a partir de "n".
float Hash(unsigned n) {
    n = (n << 13U) ^ n;
    n = n * (n * n * 15731U + 789221U) + 1376312589U;
    return static_cast<float>(n & 0x7FFFFFFFU) / 2147483648.0f;
}

// Esquina redondeada "de afuera" pintada con el color del fondo (así el
// cielo, que es rectangular, queda con las esquinas del cartel).
void MaskCorner(ImDrawList* d, ImVec2 corner, ImVec2 center, float r, float a0, float a1, ImU32 color) {
    d->PathLineTo(corner);
    d->PathArcTo(center, r, a0, a1, 10);
    d->PathFillConcave(color);
}

}  // namespace

void Draw(ImDrawList* d, ImVec2 min, ImVec2 max, float px, float rounding, ImU32 background, Mode mode) {
    const float hour = mode == Mode::Day ? 13.0f : mode == Mode::Sunset ? 18.2f : mode == Mode::Night ? 23.0f : CurrentHour();
    const Palette p = PaletteAt(hour);
    const float time = static_cast<float>(ImGui::GetTime());
    const float w = max.x - min.x;
    const float h = max.y - min.y;
    px = std::max(2.0f, std::floor(px));
    auto snap = [&](float v) { return std::floor(v / px) * px; };

    d->PushClipRect(min, max, true);

    // 1. Degradado en franjas de 2 píxeles (pixelado).
    const float groundH = snap(h * 0.11f);
    const float horizonY = max.y - groundH;
    for (float y = min.y; y < horizonY; y += px * 2.0f) {
        const float v = std::clamp((y - min.y) / std::max(1.0f, horizonY - min.y), 0.0f, 1.0f);
        const Rgb c = v < 0.55f ? Lerp(p.top, p.middle, v / 0.55f) : Lerp(p.middle, p.horizon, (v - 0.55f) / 0.45f);
        d->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, std::min(horizonY, y + px * 2.0f)), U32(c));
    }

    // 2. Estrellas que titilan.
    if (p.stars > 0.01f) {
        const int count = static_cast<int>(w * h / (px * px * 260.0f));
        for (int i = 0; i < count; ++i) {
            const float x = min.x + snap(Hash(i * 3 + 1) * w);
            const float y = min.y + snap(Hash(i * 3 + 2) * (horizonY - min.y) * 0.75f);
            const float twinkle = 0.55f + 0.45f * std::sin(time * (1.0f + Hash(i * 3 + 3) * 2.0f) + i);
            const float size = Hash(i * 7) > 0.85f ? px * 2.0f : px;
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), U32(Hex(0xFFFFFF), p.stars * twinkle * 0.85f));
        }
    }

    // 3. Sol (de día) o luna (de noche), cuadrados como en un juego de bloques.
    {
        const bool isDay = hour >= 6.0f && hour < 19.5f;
        const float progress = isDay ? (hour - 6.0f) / 13.5f
                                     : std::fmod(hour - 19.5f + 24.0f, 24.0f) / 10.5f;  // 19:30 -> 6:00
        const float x = min.x + snap(w * (0.10f + 0.80f * progress));
        const float arc = std::sin(std::clamp(progress, 0.0f, 1.0f) * kPi);
        const float size = snap(std::clamp(h * 0.16f, px * 8.0f, px * 16.0f));
        const float y = snap(horizonY - size * 0.7f - arc * (horizonY - min.y) * 0.62f);
        if (isDay) {
            const float ring = px * 2.0f;
            d->AddRectFilled(ImVec2(x - ring, y - ring), ImVec2(x + size + ring, y + size + ring), U32(Hex(0xFFD24A), 0.25f));
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), U32(Hex(0xFFD24A)));
            d->AddRectFilled(ImVec2(x + px * 2, y + px * 2), ImVec2(x + size - px * 2, y + size - px * 2), U32(Hex(0xFFF4BF)));
        } else {
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), U32(Hex(0xE6EAF5)));
            // cráteres
            d->AddRectFilled(ImVec2(x + px * 2, y + px * 2), ImVec2(x + px * 4, y + px * 4), U32(Hex(0xC4CADB)));
            d->AddRectFilled(ImVec2(x + size - px * 5, y + size - px * 4), ImVec2(x + size - px * 2, y + size - px * 2),
                             U32(Hex(0xC4CADB)));
            d->AddRectFilled(ImVec2(x + px * 5, y + size - px * 6), ImVec2(x + px * 7, y + size - px * 5), U32(Hex(0xC4CADB)));
        }
    }

    // 4. Nubes de bloques que avanzan despacio (cada una a su velocidad).
    {
        constexpr int kClouds = 7;
        for (int i = 0; i < kClouds; ++i) {
            const float cw = snap(px * (12.0f + Hash(100 + i) * 14.0f));   // ancho
            const float ch = px * 3.0f;                                     // alto de la parte de abajo
            const float speed = px * (0.6f + Hash(200 + i) * 1.2f);        // píxeles por segundo
            const float span = w + cw * 2.0f;
            const float x = min.x - cw + snap(std::fmod(Hash(300 + i) * span + time * speed, span));
            const float y = min.y + snap((horizonY - min.y) * (0.10f + Hash(400 + i) * 0.55f));
            // parte de abajo (ancha) + parte de arriba (más angosta, corrida) + borde oscuro abajo
            d->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + ch), U32(p.cloud));
            const float topW = snap(cw * (0.45f + Hash(500 + i) * 0.2f));
            const float topX = x + snap((cw - topW) * Hash(600 + i));
            d->AddRectFilled(ImVec2(topX, y - px * 2.0f), ImVec2(topX + topW, y), U32(p.cloud));
            d->AddRectFilled(ImVec2(x, y + ch), ImVec2(x + cw, y + ch + px), U32(p.cloudShade));
        }
    }

    // 5. Suelo: una silueta escalonada (columnas de 4 píxeles de ancho).
    {
        const float column = px * 4.0f;
        for (float x = min.x; x < max.x; x += column) {
            const unsigned index = static_cast<unsigned>((x - min.x) / column);
            const float bump = snap(px * (Hash(700 + index / 3) * 3.0f + Hash(800 + index) * 1.5f));
            const float top = horizonY - bump;
            d->AddRectFilled(ImVec2(x, top), ImVec2(x + column, top + px), U32(p.groundTop));
            d->AddRectFilled(ImVec2(x, top + px), ImVec2(x + column, max.y), U32(p.ground));
        }
    }

    d->PopClipRect();

    // 6. Esquinas redondeadas (pintando "afuera" con el color del fondo).
    if (rounding > 0.0f) {
        const float r = rounding;
        MaskCorner(d, min, ImVec2(min.x + r, min.y + r), r, 1.5f * kPi, 1.0f * kPi, background);
        MaskCorner(d, ImVec2(max.x, min.y), ImVec2(max.x - r, min.y + r), r, 2.0f * kPi, 1.5f * kPi, background);
        MaskCorner(d, max, ImVec2(max.x - r, max.y - r), r, 0.5f * kPi, 0.0f, background);
        MaskCorner(d, ImVec2(min.x, max.y), ImVec2(min.x + r, max.y - r), r, 1.0f * kPi, 0.5f * kPi, background);
    }
}

}  // namespace luma::launcher::gui::sky
