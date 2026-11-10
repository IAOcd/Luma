// =============================================================================
//  textures.cpp - Cargar imágenes como texturas (ver textures.h)
// =============================================================================
#include "gui/textures.h"

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <map>
#include <vector>

namespace luma::launcher::gui::textures {

namespace {

ID3D11Device* g_device = nullptr;

struct Entry {
    bool tried = false;
    Texture texture;
    ID3D11ShaderResourceView* view = nullptr;
};
std::map<std::wstring, Entry> g_cache;  // solo el hilo de la interfaz la usa

// Decodifica con WIC a RGBA (4 bytes por píxel).
bool Decode(const std::wstring& path, std::vector<std::uint8_t>& rgba, UINT& w, UINT& h) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 &&
              w <= 4096 && h <= 4096 && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom));
    if (ok) {
        rgba.resize(static_cast<std::size_t>(w) * h * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return ok;
}

}  // namespace

void SetDevice(ID3D11Device* device) {
    g_device = device;
}

const Texture* Get(const std::wstring& path) {
    Entry& e = g_cache[path];
    if (!e.tried) {
        e.tried = true;
        std::vector<std::uint8_t> rgba;
        UINT w = 0;
        UINT h = 0;
        if (g_device != nullptr && Decode(path, rgba, w, h)) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{rgba.data(), w * 4, 0};
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture))) {
                g_device->CreateShaderResourceView(texture, nullptr, &e.view);
                texture->Release();
            }
            if (e.view != nullptr) {
                e.texture.id = reinterpret_cast<ImTextureID>(e.view);
                e.texture.width = static_cast<int>(w);
                e.texture.height = static_cast<int>(h);
            }
        }
    }
    return e.view != nullptr ? &e.texture : nullptr;
}

void ReleaseAll() {
    for (auto& [path, e] : g_cache) {
        if (e.view != nullptr) e.view->Release();
    }
    g_cache.clear();
}

}  // namespace luma::launcher::gui::textures
