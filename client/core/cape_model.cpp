// =============================================================================
//  cape_model.cpp - La capa 3D de Luma (la dibuja Luma, no la skin del juego)
// =============================================================================
//  POR QUÉ: dentro de la skin, el juego solo acepta capas de 64x32 y sin
//  animación (lo probamos: una de 128x64 ni aparece). Para que se vean en alta
//  calidad y animadas, Luma dibuja la capa ella misma, dentro del mundo, con
//  las herramientas del propio juego (sdk/draw3d: triángulos con textura que
//  las paredes tapan, como cualquier cosa del mundo).
//
//  CÓMO:
//    1. Las imágenes de las capas se guardan en %APPDATA%\Luma\capes\ (el
//       juego carga texturas desde un archivo; ver sdk/draw3d).
//    2. En cada frame (BuildCape, hilo de render) armamos la "caja" de la
//       capa (10 x 16 x 1 píxeles del modelo) colgada de tus hombros, mirando
//       hacia atrás, inclinada según cuánto te mueves (como una capa de verdad).
//    3. Cada cara de la caja lleva su parte de la imagen (la forma estándar
//       de las capas de Minecraft). La cara de afuera de las animadas sale de
//       otra imagen con todos los cuadros, y va cambiando de cuadro.
//
//  Por ahora: tu propia capa, en tercera persona (en primera persona tu
//  espalda no se ve). Las de otros usuarios de Luma: siguiente paso.
// =============================================================================
#include "core/cape_model.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/cosmetics.h"
#include "core/logger.h"
#include "core/paths.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"
#include "sdk/player.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace luma::cosmetics::model {

namespace {

namespace fs = std::filesystem;

std::atomic<int> g_capeIndex{-1};  // la capa que se dibuja (índice en kCapes; -1 = ninguna)
std::string g_fullPath[std::size(kCapes)];  // (se llenan en PrepareFiles, antes de dibujar)
std::string g_animPath[std::size(kCapes)];
std::atomic<bool> g_filesReady{false};

fs::path CapesDir() {
    return paths::DataDir() / L"capes";
}

// --- Guardar las imágenes en el disco ---------------------------------------------------
bool ResourceBytes(int resource, const void*& bytes, DWORD& size) {
    HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // 10 = RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    bytes = loaded ? LockResource(loaded) : nullptr;
    size = found ? SizeofResource(module, found) : 0;
    return bytes != nullptr && size > 0;
}

// Escribe un archivo solo si no existe o cambió de tamaño (no en cada inicio).
bool WriteIfChanged(const fs::path& path, const void* bytes, std::size_t size) {
    std::error_code ec;
    if (fs::exists(path, ec) && fs::file_size(path, ec) == size) return true;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

// Codifica una imagen RGBA como PNG con WIC (para las capas dibujadas en código).
bool EncodePng(const fs::path& path, const std::vector<std::uint8_t>& rgba, UINT w, UINT h) {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        (format == GUID_WICPixelFormat32bppRGBA || format == GUID_WICPixelFormat32bppBGRA)) {
        // El codificador PNG puede pedir los colores en orden BGRA (azul primero).
        std::vector<std::uint8_t> pixels = rgba;
        if (format == GUID_WICPixelFormat32bppBGRA) {
            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
        }
        ok = SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(pixels.size()), pixels.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (!ok) log::Warn("Capas 3D: no se pudo guardar {}.", path.string());
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(comInit)) CoUninitialize();
    return ok;
}

// La capa dibujada en código (64x32) agrandada 4 veces, píxel por píxel: así
// se ve nítida aunque el juego suavice la textura al dibujarla.
std::vector<std::uint8_t> Upscale4(const std::vector<std::uint8_t>& src) {
    constexpr int k = 4, w = 64, h = 32;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w * k) * h * k * 4);
    for (int y = 0; y < h * k; ++y) {
        for (int x = 0; x < w * k; ++x) {
            std::memcpy(&out[(static_cast<std::size_t>(y) * w * k + x) * 4], &src[(static_cast<std::size_t>(y / k) * w + x / k) * 4], 4);
        }
    }
    return out;
}

// --- La forma de la capa ---------------------------------------------------------------
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

// Un rectángulo de la imagen, en las unidades de una capa de 64x32.
struct Rect {
    float u0, v0, u1, v1;
};

// Agrega un cuadrilátero (a-b-c-d, en orden) con su parte de la imagen, de
// los dos lados (así se ve desde donde sea, sin depender de hacia dónde
// "mira" cada triángulo).
void Quad(std::vector<float>& out, V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1) {
    const V3 p[4] = {a, b, c, d};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    constexpr int kOrder[12] = {0, 1, 2, 0, 2, 3, 2, 1, 0, 3, 2, 0};
    for (int i : kOrder) {
        out.insert(out.end(), {p[i].x, p[i].y, p[i].z, uv[i][0], uv[i][1]});
    }
}

void QuadRect(std::vector<float>& out, V3 a, V3 b, V3 c, V3 d, Rect r) {
    Quad(out, a, b, c, d, r.u0 / 64.0f, r.v0 / 32.0f, r.u1 / 64.0f, r.v1 / 32.0f);
}

float Wrap(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

// Lo que se mueve suave de un frame al otro (hilo de render).
float g_bodyYaw = 0.0f;
float g_swing = 6.0f;
bool g_haveBody = false;
LARGE_INTEGER g_lastTime{};

void BuildCape(float partialTick, std::vector<sdk::draw3d::TexturedTriangles>& out) {
    const int index = g_capeIndex.load(std::memory_order_relaxed);
    if (index < 0 || !g_filesReady.load(std::memory_order_relaxed)) return;
    // En primera persona tu espalda no se ve (salvo con el free cam).
    if (sdk::camera::CurrentPerspective() <= 0 && !sdk::camera::FreecamActive()) return;
    sdk::player::Snapshot p;
    if (!sdk::player::Read(p)) return;

    // Tiempo desde el frame anterior (para que el movimiento sea suave).
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_lastTime.QuadPart == 0 ? 0.0f
                                        : static_cast<float>(now.QuadPart - g_lastTime.QuadPart) / static_cast<float>(freq.QuadPart);
    g_lastTime = now;
    dt = std::clamp(dt, 0.0f, 0.1f);

    // Dónde estás en ESTE frame: entre el tick anterior y el actual.
    const float t = std::clamp(partialTick, 0.0f, 1.0f);
    const V3 eye{p.eyeOld[0] + (p.eye[0] - p.eyeOld[0]) * t, p.eyeOld[1] + (p.eye[1] - p.eyeOld[1]) * t,
                 p.eyeOld[2] + (p.eye[2] - p.eyeOld[2]) * t};
    constexpr float kEyeHeight = 1.62f;
    const V3 feet{eye.x, eye.y - kEyeHeight, eye.z};

    // Hacia dónde mira el CUERPO: como en el juego, sigue a la cabeza pero sin
    // separarse más de 50 grados, y al caminar gira hacia donde vas.
    constexpr float kDeg = 3.14159265f / 180.0f;
    const float horizontal = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[2] * p.velocity[2]);
    if (!g_haveBody) {
        g_bodyYaw = p.yaw;
        g_haveBody = true;
    }
    float target = g_bodyYaw;
    if (horizontal > 0.02f) {
        const float moveYaw = std::atan2(-p.velocity[0], p.velocity[2]) / kDeg;
        // (caminando hacia atrás, el cuerpo sigue mirando hacia adelante)
        target = std::fabs(Wrap(moveYaw - p.yaw)) > 100.0f ? p.yaw : moveYaw;
    }
    float diff = Wrap(p.yaw - target);
    if (diff > 50.0f) target = p.yaw - 50.0f;
    if (diff < -50.0f) target = p.yaw + 50.0f;
    g_bodyYaw += Wrap(target - g_bodyYaw) * (1.0f - std::exp(-12.0f * dt));
    diff = Wrap(p.yaw - g_bodyYaw);
    if (diff > 50.0f) g_bodyYaw = p.yaw - 50.0f;
    if (diff < -50.0f) g_bodyYaw = p.yaw + 50.0f;

    // Direcciones del cuerpo (convención del juego: 0 = sur (+Z), 90 = oeste (-X)).
    const float sy = std::sin(g_bodyYaw * kDeg), cy = std::cos(g_bodyYaw * kDeg);
    const V3 forward{-sy, 0.0f, cy};
    const V3 back{sy, 0.0f, -cy};
    const V3 right{-cy, 0.0f, -sy};
    const V3 up{0.0f, 1.0f, 0.0f};

    // Cuánto se levanta la capa: un poco quieta, más al caminar hacia
    // adelante y al caer (como en Java).
    const float forwardSpeed = p.velocity[0] * forward.x + p.velocity[2] * forward.z;  // bloques por tick
    float targetSwing = 6.0f + std::clamp(forwardSpeed * 280.0f, 0.0f, 70.0f);
    if (p.velocity[1] < -0.1f) targetSwing += std::clamp(-p.velocity[1] * 40.0f, 0.0f, 20.0f);
    g_swing += (targetSwing - g_swing) * (1.0f - std::exp(-6.0f * dt));
    const float th = g_swing * kDeg;
    const V3 down = up * -std::cos(th) + back * std::sin(th);  // a lo largo de la capa (hacia abajo)
    const V3 outward = up * std::sin(th) + back * std::cos(th);  // hacia afuera (lo que ven los de atrás)

    // Medidas del modelo del jugador: 1 píxel = 1/16 de bloque x 0,9375.
    constexpr float px = 0.9375f / 16.0f;
    const V3 hinge = feet + up * (24.0f * px) + back * (2.0f * px);  // arriba de la espalda
    const V3 l = right * (-5.0f * px), r = right * (5.0f * px);
    const V3 len = down * (16.0f * px);
    const V3 thick = outward * (1.0f * px);
    // Las 8 esquinas: adentro (contra la espalda) y afuera.
    const V3 iTL = hinge + l, iTR = hinge + r, iBL = hinge + l + len, iBR = hinge + r + len;
    const V3 oTL = iTL + thick, oTR = iTR + thick, oBL = iBL + thick, oBR = iBR + thick;

    const CapeDesign& design = kCapes[index];
    sdk::draw3d::TexturedTriangles body;
    body.texture = g_fullPath[index];
    // Afuera: (1,1)-(11,17). Mirando desde atrás, el lado izquierdo de la
    // imagen queda a TU izquierda.
    const bool animated = design.animResource != 0 && !g_animPath[index].empty();
    if (!animated) QuadRect(body.vertices, oTL, oTR, oBR, oBL, {1, 1, 11, 17});
    // Adentro (contra la espalda): (12,1)-(22,17), al revés.
    QuadRect(body.vertices, iTR, iTL, iBL, iBR, {12, 1, 22, 17});
    // Costados, arriba y abajo.
    QuadRect(body.vertices, iTL, oTL, oBL, iBL, {0, 1, 1, 17});
    QuadRect(body.vertices, oTR, iTR, iBR, oBR, {11, 1, 12, 17});
    QuadRect(body.vertices, iTL, iTR, oTR, oTL, {1, 0, 11, 1});
    QuadRect(body.vertices, oBL, oBR, iBR, iBL, {11, 0, 21, 1});
    out.push_back(std::move(body));

    if (animated) {
        // El cuadro de AHORA de la animación, de la grilla de cuadros.
        const ULONGLONG ms = GetTickCount64();
        const int frame = static_cast<int>((ms / static_cast<ULONGLONG>(std::max(design.frameMs, 1))) %
                                           static_cast<ULONGLONG>(std::max(design.frames, 1)));
        const int rows = (design.frames + design.columns - 1) / design.columns;
        const float cw = 1.0f / static_cast<float>(design.columns), ch = 1.0f / static_cast<float>(rows);
        const float u0 = (frame % design.columns) * cw, v0 = (frame / design.columns) * ch;
        sdk::draw3d::TexturedTriangles face;
        face.texture = g_animPath[index];
        Quad(face.vertices, oTL, oTR, oBR, oBL, u0, v0, u0 + cw, v0 + ch);
        out.push_back(std::move(face));
    }
}

}  // namespace

void PrepareFiles() {
    std::error_code ec;
    fs::create_directories(CapesDir(), ec);
    int written = 0;
    for (std::size_t i = 0; i < std::size(kCapes); ++i) {
        const CapeDesign& c = kCapes[i];
        const fs::path full = CapesDir() / (std::string(c.id) + ".png");
        bool ok = false;
        if (c.resource != 0) {
            const void* bytes = nullptr;
            DWORD size = 0;
            ok = ResourceBytes(c.resource, bytes, size) && WriteIfChanged(full, bytes, size);
        } else {
            const CapeTexture drawn = CapeImage(c.id);
            ok = !drawn.rgba.empty() && EncodePng(full, Upscale4(drawn.rgba), 256, 128);
        }
        g_fullPath[i] = ok ? full.string() : std::string();
        if (c.animResource != 0) {
            const fs::path anim = CapesDir() / (std::string(c.id) + "_anim.png");
            const void* bytes = nullptr;
            DWORD size = 0;
            g_animPath[i] = ResourceBytes(c.animResource, bytes, size) && WriteIfChanged(anim, bytes, size)
                                ? anim.string()
                                : std::string();
        }
        written += ok ? 1 : 0;
    }
    g_filesReady.store(true);
    log::Info("Capas 3D: {} imágenes listas en {}.", written, CapesDir().string());
}

void Start() {
    if (!sdk::draw3d::InitTextures()) return;
    sdk::draw3d::SetTexturedProvider(&BuildCape);
}

void Stop() {
    sdk::draw3d::SetTexturedProvider(nullptr);
}

void SetCape(const std::string& id) {
    int index = -1;
    for (std::size_t i = 0; i < std::size(kCapes); ++i) {
        if (id == kCapes[i].id) index = static_cast<int>(i);
    }
    if (index >= 0 && g_fullPath[index].empty()) index = -1;  // (sin imagen en el disco: nada)
    g_capeIndex.store(index);
}

}  // namespace luma::cosmetics::model
