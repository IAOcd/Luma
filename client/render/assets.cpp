// =============================================================================
//  assets.cpp - Decodificar el logo y subirlo a la GPU
// =============================================================================
#include "render/assets.h"

#include <windows.h>
#include <wincodec.h>  // WIC: decodificar PNG
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/logger.h"
#include "render/gpu.h"

using Microsoft::WRL::ComPtr;

// Inicio de nuestro propio módulo (lo define el enlazador de Microsoft).
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace luma::render::assets {

namespace {

constexpr int kLogoResourceId = 101;  // ver client/assets/luma.rc

std::mutex g_mutex;
std::vector<std::uint8_t> g_logoPixels;  // RGBA decodificado, listo para subir
UINT g_logoWidth = 0;
UINT g_logoHeight = 0;
bool g_logoPending = false;
ImTextureID g_logo = 0;
float g_logoAspect = 1.0f;

// Convierte el PNG (L carbón sobre fondo marfil) en una máscara: blanco con
// transparencia según qué tan "oscuro" es cada píxel. Fondo marfil -> 0,
// L carbón -> 1, bordes suaves en el medio. Después recorta los márgenes.
void BuildMask(std::vector<std::uint8_t>& rgba, UINT& width, UINT& height) {
    constexpr float kBackground = 0.90f;  // luminosidad del marfil
    constexpr float kForeground = 0.16f;  // luminosidad del carbón
    UINT minX = width, minY = height, maxX = 0, maxY = 0;
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            std::uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            const float luminance = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
            float alpha = std::clamp((kBackground - luminance) / (kBackground - kForeground), 0.0f, 1.0f);
            alpha *= p[3] / 255.0f;  // las esquinas transparentes del PNG siguen transparentes
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<std::uint8_t>(alpha * 255.0f + 0.5f);
            if (p[3] > 12) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX <= minX || maxY <= minY) {
        return;
    }
    // Recortar a la L con un pequeño margen.
    const UINT pad = 4;
    minX = minX > pad ? minX - pad : 0;
    minY = minY > pad ? minY - pad : 0;
    maxX = std::min(width - 1, maxX + pad);
    maxY = std::min(height - 1, maxY + pad);
    const UINT newWidth = maxX - minX + 1;
    const UINT newHeight = maxY - minY + 1;
    std::vector<std::uint8_t> cropped(static_cast<size_t>(newWidth) * newHeight * 4);
    for (UINT y = 0; y < newHeight; ++y) {
        std::copy_n(&rgba[(static_cast<size_t>(y + minY) * width + minX) * 4], newWidth * 4,
                    &cropped[static_cast<size_t>(y) * newWidth * 4]);
    }
    rgba = std::move(cropped);
    width = newWidth;
    height = newHeight;
}

}  // namespace

bool Decode() {
    // 1. Encontrar los bytes del PNG dentro de nuestra propia DLL.
    HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kLogoResourceId), MAKEINTRESOURCEW(10));  // 10 = RT_RCDATA
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    if (bytes == nullptr || size == 0) {
        log::Warn("No se encontró el logo dentro de Luma.dll.");
        return false;
    }

    // 2. Decodificar con WIC. WIC es una API "COM": hay que iniciar COM en
    //    este hilo (el de Luma, no uno del juego) y cerrarlo al terminar.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapSource> converted;
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), size)) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                       &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted)) &&
            SUCCEEDED(converted->GetSize(&width, &height)) && width > 0 && height > 0) {
            std::vector<std::uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            if (SUCCEEDED(converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()),
                                                pixels.data()))) {
                BuildMask(pixels, width, height);
                std::lock_guard lock(g_mutex);
                g_logoPixels = std::move(pixels);
                g_logoWidth = width;
                g_logoHeight = height;
                g_logoPending = true;
                ok = true;
            }
        }
    }
    if (SUCCEEDED(comInit)) {
        CoUninitialize();
    }
    if (!ok) {
        log::Warn("No se pudo decodificar el logo.");
    }
    return ok;
}

void UploadPending() {
    std::lock_guard lock(g_mutex);
    if (!g_logoPending) {
        return;
    }
    g_logoPending = false;
    g_logo = gpu::CreateTexture(g_logoPixels.data(), g_logoWidth, g_logoHeight);
    g_logoAspect = static_cast<float>(g_logoWidth) / static_cast<float>(std::max(1u, g_logoHeight));
    g_logoPixels.clear();
    g_logoPixels.shrink_to_fit();
    if (g_logo != 0) {
        log::Info("Logo cargado ({}x{}).", g_logoWidth, g_logoHeight);
    }
}

ImTextureID Logo() {
    return g_logo;
}

float LogoAspect() {
    return g_logoAspect;
}

void DrawLogo(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
    if (g_logo != 0) {
        // Encajar la L (que es más ancha que alta) dentro del cuadrado.
        float w = size;
        float h = size;
        if (g_logoAspect >= 1.0f) {
            h = size / g_logoAspect;
        } else {
            w = size * g_logoAspect;
        }
        draw->AddImage(g_logo, ImVec2(center.x - w * 0.5f, center.y - h * 0.5f),
                       ImVec2(center.x + w * 0.5f, center.y + h * 0.5f), ImVec2(0, 0), ImVec2(1, 1), color);
        return;
    }
    // Respaldo: una L redondeada dibujada a mano.
    const float t = size * 0.22f;
    draw->AddRectFilled(ImVec2(center.x - size * 0.4f, center.y - size * 0.4f),
                        ImVec2(center.x - size * 0.4f + t, center.y + size * 0.3f), color, t * 0.5f);
    draw->AddRectFilled(ImVec2(center.x - size * 0.4f, center.y + size * 0.3f - t),
                        ImVec2(center.x + size * 0.4f, center.y + size * 0.3f), color, t * 0.5f);
}

}  // namespace luma::render::assets
