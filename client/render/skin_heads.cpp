// =============================================================================
//  skin_heads.cpp - Caras de skins (ver skin_heads.h)
// =============================================================================
#include "render/skin_heads.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <map>
#include <mutex>
#include <vector>

#include "render/gpu.h"

namespace luma::render::skinheads {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kScale = 16;           // cada píxel de la cara -> un bloque de 16x16
constexpr int kSize = 8 * kScale;    // 128x128

std::mutex g_mutex;
std::vector<std::uint8_t> g_pendingOwn;  // la cara nueva esperando subir a la GPU
ImTextureID g_own = 0;
struct File {
    bool requested = false;          // la interfaz la pidió
    bool decoded = false;            // el hilo de Luma ya la leyó (bien o mal)
    std::vector<std::uint8_t> face;  // lista para subir a la GPU
    ImTextureID texture = 0;
};
std::map<std::wstring, File> g_files;

// Recorta la cara (frente de la cabeza + segunda capa) y la agranda.
bool Compose(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t>& out) {
    if (rgba == nullptr || w < 64 || w % 64 != 0 || (h != w && h * 2 != w)) return false;
    const std::uint32_t k = w / 64;
    auto pixel = [&](std::uint32_t x, std::uint32_t y) { return rgba + ((y * k) * w + x * k) * 4; };
    out.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t* base = pixel(8 + x, 8 + y);
            const std::uint8_t* hat = pixel(40 + x, 8 + y);
            // mezclar la segunda capa encima (según su transparencia)
            const float a = hat[3] / 255.0f;
            std::uint8_t c[4];
            for (int i = 0; i < 3; ++i) c[i] = static_cast<std::uint8_t>(base[i] * (1.0f - a) + hat[i] * a);
            c[3] = 255;
            for (int dy = 0; dy < kScale; ++dy) {
                for (int dx = 0; dx < kScale; ++dx) {
                    std::uint8_t* o = &out[((y * kScale + dy) * kSize + (x * kScale + dx)) * 4];
                    o[0] = c[0];
                    o[1] = c[1];
                    o[2] = c[2];
                    o[3] = c[3];
                }
            }
        }
    }
    return true;
}

bool DecodeFile(const std::wstring& path, std::vector<std::uint8_t>& rgba, UINT& w, UINT& h) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> converted;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted)) ||
        FAILED(converted->GetSize(&w, &h)) || w == 0 || h == 0 || w > 1024 || h > 1024) {
        return false;
    }
    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    return SUCCEEDED(converted->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()));
}

}  // namespace

void SetOwn(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> face;
    if (!Compose(rgba, width, height, face)) return;
    std::lock_guard lock(g_mutex);
    g_pendingOwn = std::move(face);
}

void LoadOwnFromFile(const std::wstring& path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::uint8_t> rgba;
    UINT w = 0;
    UINT h = 0;
    if (DecodeFile(path, rgba, w, h)) {
        SetOwn(rgba.data(), w, h);
    }
}

ImTextureID Own() {
    std::vector<std::uint8_t> face;
    {
        std::lock_guard lock(g_mutex);
        face.swap(g_pendingOwn);
    }
    if (!face.empty()) {
        const ImTextureID id = gpu::CreateTexture(face.data(), kSize, kSize);
        if (id != 0) g_own = id;
    }
    return g_own;
}

ImTextureID FromFile(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    File& f = g_files[path];
    if (!f.requested) {
        f.requested = true;  // el hilo de Luma (Poll) la va a leer
        return 0;
    }
    if (!f.face.empty()) {
        f.texture = gpu::CreateTexture(f.face.data(), kSize, kSize);
        f.face.clear();
        f.face.shrink_to_fit();
    }
    return f.texture;
}

void Poll() {
    // Leer los PNG pedidos (fuera del lock: leer el disco es lento).
    std::vector<std::wstring> todo;
    {
        std::lock_guard lock(g_mutex);
        for (auto& [path, f] : g_files) {
            if (f.requested && !f.decoded) {
                f.decoded = true;
                todo.push_back(path);
            }
        }
    }
    if (todo.empty()) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // en el hilo de Luma (no en los del juego)
    for (const std::wstring& path : todo) {
        std::vector<std::uint8_t> rgba;
        std::vector<std::uint8_t> face;
        UINT w = 0;
        UINT h = 0;
        if (DecodeFile(path, rgba, w, h) && Compose(rgba.data(), w, h, face)) {
            std::lock_guard lock(g_mutex);
            g_files[path].face = std::move(face);
        }
    }
}

}  // namespace luma::render::skinheads
