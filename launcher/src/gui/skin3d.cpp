// =============================================================================
//  skin3d.cpp - El personaje en 3D (ver skin3d.h)
// =============================================================================
#include "gui/skin3d.h"

#include <d3d11.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <cmath>

namespace luma::launcher::gui::skin3d {

namespace {

ID3D11SamplerState* g_point = nullptr;

// Callback de ImGui: antes de dibujar el personaje, cambiar al filtro "punto".
void UsePointFilter(const ImDrawList*, const ImDrawCmd*) {
    auto* rs = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (rs != nullptr && g_point != nullptr) {
        rs->DeviceContext->PSSetSamplers(0, 1, &g_point);
    }
}

struct V3 {
    float x, y, z;
};

V3 RotX(V3 p, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {p.x, p.y * c - p.z * s, p.y * s + p.z * c};
}
V3 RotY(V3 p, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
}
V3 RotZ(V3 p, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {p.x * c - p.y * s, p.x * s + p.y * c, p.z};
}

// Una caja del modelo, en "píxeles de skin" (el personaje mide 32 de alto).
struct Box {
    float x0, y0, z0, x1, y1, z1;
    int u, v;             // dónde empieza su rectángulo en la imagen
    float inflate;        // la segunda capa es un poco más grande
    V3 pivot;             // punto sobre el que gira (hombro, cadera...)
    float swingX;         // giro hacia adelante/atrás (piernas)
    float swingZ;         // giro hacia los costados (brazos)
    bool mirror;          // skins viejas: el lado izquierdo usa la textura del derecho espejada
};

// Una cara lista para dibujar.
struct Face {
    ImVec2 p[4];
    ImVec2 uv[4];
    float depth;
    float shade;
};

constexpr int kMaxFaces = 14 * 6;  // 7 cajas + 7 de la segunda capa, 6 caras cada una
Face g_faces[kMaxFaces];

}  // namespace

void Init(ID3D11Device* device) {
    if (device == nullptr || g_point != nullptr) return;
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&desc, &g_point);
}

void Shutdown() {
    if (g_point != nullptr) {
        g_point->Release();
        g_point = nullptr;
    }
}

bool DetectSlim(const unsigned char* rgba, int width, int height) {
    if (rgba == nullptr || width < 64 || height < 64 || height != width) return false;  // las de 64x32 nunca son finas
    const int k = width / 64;
    auto alpha = [&](int x, int y) { return rgba[((y * k) * width + x * k) * 4 + 3]; };
    // En las skins de brazos finos, las columnas 54-55 del brazo derecho (filas 20-31) quedan vacías.
    return alpha(54, 20) == 0 && alpha(55, 20) == 0 && alpha(54, 31) == 0 && alpha(55, 31) == 0;
}

ImVec2 Draw(ImDrawList* d, ImVec2 min, ImVec2 max, const Model& model, const View& view) {
    const float arm = model.slim ? 3.0f : 4.0f;
    const float t = view.time;
    // Animación en reposo: los brazos se balancean apenas hacia los costados y
    // las piernas casi nada (como el personaje del menú del juego).
    const float sway = std::sin(t * 1.3f) * 0.05f + 0.04f;
    const float step = std::sin(t * 0.9f) * 0.035f;
    const bool legacy = model.legacy;

    // Las 7 cajas base + su segunda capa (las skins viejas solo tienen sombrero).
    Box boxes[14];
    int n = 0;
    auto add = [&](float x0, float y0, float z0, float x1, float y1, float z1, int u, int v, float inflate, V3 pivot,
                   float sx, float sz, bool mirror) {
        boxes[n++] = {x0, y0, z0, x1, y1, z1, u, v, inflate, pivot, sx, sz, mirror};
    };
    const V3 neck{0, 24, 0};
    const V3 rShoulder{-4.0f - arm * 0.5f, 22, 0};
    const V3 lShoulder{4.0f + arm * 0.5f, 22, 0};
    const V3 rHip{-2, 12, 0};
    const V3 lHip{2, 12, 0};
    // cabeza + sombrero
    add(-4, 24, -4, 4, 32, 4, 0, 0, 0.0f, neck, 0, 0, false);
    add(-4, 24, -4, 4, 32, 4, 32, 0, 0.5f, neck, 0, 0, false);
    // cuerpo
    add(-4, 12, -2, 4, 24, 2, 16, 16, 0.0f, neck, 0, 0, false);
    // brazo derecho (a la izquierda de la pantalla) e izquierdo
    add(-4 - arm, 12, -2, -4, 24, 2, 40, 16, 0.0f, rShoulder, 0, -sway, false);
    add(4, 12, -2, 4 + arm, 24, 2, legacy ? 40 : 32, legacy ? 16 : 48, 0.0f, lShoulder, 0, sway, legacy);
    // piernas
    add(-4, 0, -2, 0, 12, 2, 0, 16, 0.0f, rHip, step, 0, false);
    add(0, 0, -2, 4, 12, 2, legacy ? 0 : 16, legacy ? 16 : 48, 0.0f, lHip, -step, 0, legacy);
    if (!legacy) {
        add(-4, 12, -2, 4, 24, 2, 16, 32, 0.25f, neck, 0, 0, false);                       // chaqueta
        add(-4 - arm, 12, -2, -4, 24, 2, 40, 32, 0.25f, rShoulder, 0, -sway, false);      // manga derecha
        add(4, 12, -2, 4 + arm, 24, 2, 48, 48, 0.25f, lShoulder, 0, sway, false);         // manga izquierda
        add(-4, 0, -2, 0, 12, 2, 0, 32, 0.25f, rHip, step, 0, false);                      // pantalón derecho
        add(0, 0, -2, 4, 12, 2, 0, 48, 0.25f, lHip, -step, 0, false);                      // pantalón izquierdo
    }

    // Cámara: dónde mira y qué tan grande se ve.
    const float zoom = std::max(1.0f, view.zoom);
    const float focusY = 16.0f + (28.0f - 16.0f) * std::clamp((zoom - 1.0f) / 1.5f, 0.0f, 1.0f);
    const float scale = (max.y - min.y) / 34.0f * zoom;
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    constexpr float kDistance = 90.0f;  // distancia de la cámara (en píxeles de skin): perspectiva suave
    const float texH = legacy ? 32.0f : 64.0f;

    auto project = [&](V3 p, float& depth) {
        p = RotX(RotY(p, view.yaw), view.pitch);
        depth = p.z;
        const float f = kDistance / (kDistance - p.z);
        return ImVec2(center.x + p.x * scale * f, center.y - (p.y - focusY) * scale * f);
    };

    int faces = 0;
    ImVec2 headTop = center;
    for (int i = 0; i < n; ++i) {
        const Box& b = boxes[i];
        const float w = b.x1 - b.x0, h = b.y1 - b.y0, dd = b.z1 - b.z0;
        const float X0 = b.x0 - b.inflate, X1 = b.x1 + b.inflate;
        const float Y0 = b.y0 - b.inflate, Y1 = b.y1 + b.inflate;
        const float Z0 = b.z0 - b.inflate, Z1 = b.z1 + b.inflate;
        // Los 6 rectángulos de la imagen (u, v, ancho, alto) según el mapa estándar.
        struct Rect {
            float u, v, w, h;
        };
        const float u = static_cast<float>(b.u), v = static_cast<float>(b.v);
        const Rect front{u + dd, v + dd, w, h}, back{u + dd + w + dd, v + dd, w, h};
        const Rect right{u, v + dd, dd, h}, left{u + dd + w, v + dd, dd, h};
        const Rect top{u + dd, v, w, dd}, bottom{u + dd + w, v, w, dd};
        // Cada cara: 4 esquinas vistas desde afuera (arriba-izq, arriba-der, abajo-der, abajo-izq).
        struct Quad {
            V3 c[4];
            Rect r;
            float shade;
        };
        const Quad quads[6] = {
            {{{X0, Y1, Z1}, {X1, Y1, Z1}, {X1, Y0, Z1}, {X0, Y0, Z1}}, front, 0.95f},
            {{{X1, Y1, Z0}, {X0, Y1, Z0}, {X0, Y0, Z0}, {X1, Y0, Z0}}, back, 0.85f},
            {{{X0, Y1, Z0}, {X0, Y1, Z1}, {X0, Y0, Z1}, {X0, Y0, Z0}}, b.mirror ? left : right, 0.78f},
            {{{X1, Y1, Z1}, {X1, Y1, Z0}, {X1, Y0, Z0}, {X1, Y0, Z1}}, b.mirror ? right : left, 0.78f},
            {{{X0, Y1, Z0}, {X1, Y1, Z0}, {X1, Y1, Z1}, {X0, Y1, Z1}}, top, 1.0f},
            {{{X0, Y0, Z1}, {X1, Y0, Z1}, {X1, Y0, Z0}, {X0, Y0, Z0}}, bottom, 0.6f},
        };
        for (const Quad& q : quads) {
            Face& f = g_faces[faces];
            float depth = 0.0f;
            for (int k = 0; k < 4; ++k) {
                // animación de la parte (alrededor de su pivote)
                V3 p{q.c[k].x - b.pivot.x, q.c[k].y - b.pivot.y, q.c[k].z - b.pivot.z};
                p = RotZ(RotX(p, b.swingX), b.swingZ);
                p = {p.x + b.pivot.x, p.y + b.pivot.y, p.z + b.pivot.z};
                float z = 0.0f;
                f.p[k] = project(p, z);
                depth += z * 0.25f;
            }
            // ¿Mira hacia la cámara? (las esquinas giran en el sentido del reloj en pantalla)
            const float cross = (f.p[1].x - f.p[0].x) * (f.p[2].y - f.p[0].y) - (f.p[1].y - f.p[0].y) * (f.p[2].x - f.p[0].x);
            if (cross <= 0.0f) {
                continue;
            }
            // UV (con un margen mínimo para que no se "cuele" el píxel vecino).
            const float e = 0.02f;
            float u0 = (q.r.u + e) / 64.0f, u1 = (q.r.u + q.r.w - e) / 64.0f;
            const float v0 = (q.r.v + e) / texH, v1 = (q.r.v + q.r.h - e) / texH;
            if (b.mirror) std::swap(u0, u1);
            f.uv[0] = ImVec2(u0, v0);
            f.uv[1] = ImVec2(u1, v0);
            f.uv[2] = ImVec2(u1, v1);
            f.uv[3] = ImVec2(u0, v1);
            f.depth = depth + (b.inflate > 0.0f ? 0.01f : 0.0f);  // la segunda capa, encima
            f.shade = q.shade;
            ++faces;
        }
        if (i == 0) {
            float z = 0.0f;
            headTop = project({0, 33.5f, 0}, z);
        }
    }
    std::sort(g_faces, g_faces + faces, [](const Face& a, const Face& b) { return a.depth < b.depth; });

    d->AddCallback(UsePointFilter, nullptr);
    for (int i = 0; i < faces; ++i) {
        const Face& f = g_faces[i];
        const int c = static_cast<int>(255.0f * f.shade);
        d->AddImageQuad(model.texture, f.p[0], f.p[1], f.p[2], f.p[3], f.uv[0], f.uv[1], f.uv[2], f.uv[3],
                        IM_COL32(c, c, c, static_cast<int>(255.0f * view.alpha)));
    }
    d->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return headTop;
}

}  // namespace luma::launcher::gui::skin3d
