// =============================================================================
//  glass.cpp - Implementación de las superficies de vidrio
// =============================================================================
#include "render/glass.h"

#include <algorithm>

#include "render/gpu.h"
#include "render/theme.h"

namespace luma::render::glass {

namespace {

ImU32 Col(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

}  // namespace

void VerticalFade(ImDrawList* draw, int firstVertex, float topY, float bottomY, float alphaTop,
                  float alphaBottom) {
    const float height = std::max(1.0f, bottomY - topY);
    for (int i = firstVertex; i < draw->VtxBuffer.Size; ++i) {
        ImDrawVert& v = draw->VtxBuffer[i];
        const float t = std::clamp((v.pos.y - topY) / height, 0.0f, 1.0f);
        const float factor = alphaTop + (alphaBottom - alphaTop) * t;
        const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        const ImU32 newAlpha = static_cast<ImU32>(std::clamp(static_cast<float>(a) * factor, 0.0f, 255.0f));
        v.col = (v.col & ~IM_COL32_A_MASK) | (newAlpha << IM_COL32_A_SHIFT);
    }
}

void Panel(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float alpha) {
    const theme::Palette& p = theme::P();
    const theme::Settings& s = theme::Get();
    const float h = max.y - min.y;

    // 1. Sombra suave: tres capas cada vez más grandes y transparentes.
    for (int i = 3; i >= 1; --i) {
        const float grow = static_cast<float>(i) * 4.0f;
        draw->AddRectFilled(ImVec2(min.x - grow * 0.5f, min.y + grow * 0.6f),
                            ImVec2(max.x + grow * 0.5f, max.y + grow * 1.4f),
                            Col(p.shadow, alpha * 0.33f / static_cast<float>(i)), rounding + grow);
    }

    // 2. El juego desenfocado, recortado a la forma del panel. Las
    //    coordenadas de textura (uv) son la posición en pantalla dividida por
    //    el tamaño de la pantalla: así el pedazo coincide con lo que hay detrás.
    const ImTextureID blur = gpu::BlurTexture();
    if (blur != 0) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        draw->AddImageRounded(blur, min, max, ImVec2(min.x / display.x, min.y / display.y),
                              ImVec2(max.x / display.x, max.y / display.y),
                              Col(ImVec4(1, 1, 1, 1), alpha), rounding);
    }

    // 3. Tinte (más fuerte si no hay desenfoque, para que se lea bien).
    ImVec4 tint = p.glassTint;
    tint.w = blur != 0 ? s.glassOpacity : std::min(0.92f, s.glassOpacity + 0.3f);
    draw->AddRectFilled(min, max, Col(tint, alpha), rounding);

    // 4. Brillo especular arriba, que se desvanece en el primer 45% del panel.
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, Col(p.highlight, alpha), rounding);
    VerticalFade(draw, start, min.y, min.y + h * 0.45f, 1.0f, 0.0f);

    // 5. Borde que capta la luz: brillante arriba, tenue abajo.
    const int borderStart = draw->VtxBuffer.Size;
    draw->AddRect(min, max, Col(p.border, alpha), rounding, 0, 1.2f);
    VerticalFade(draw, borderStart, min.y, max.y, 1.0f, 0.35f);
}

void Surface(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, float alpha, float hover) {
    const theme::Palette& p = theme::P();
    const float h = max.y - min.y;
    ImVec4 base = p.surface;
    base.x += (p.surfaceHover.x - base.x) * hover;
    base.y += (p.surfaceHover.y - base.y) * hover;
    base.z += (p.surfaceHover.z - base.z) * hover;
    base.w += (p.surfaceHover.w - base.w) * hover;
    draw->AddRectFilled(min, max, Col(base, alpha), rounding);

    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, Col(p.highlight, alpha * (0.6f + 0.6f * hover)), rounding);
    VerticalFade(draw, start, min.y, min.y + h * 0.55f, 1.0f, 0.0f);

    const int borderStart = draw->VtxBuffer.Size;
    draw->AddRect(min, max, Col(p.border, alpha * (0.7f + 0.5f * hover)), rounding, 0, 1.0f);
    VerticalFade(draw, borderStart, min.y, max.y, 1.0f, 0.25f);
}

}  // namespace luma::render::glass
