// =============================================================================
//  textures.cpp - Cargar imágenes como texturas (ver textures.h)
// =============================================================================
#include "gui/textures.h"

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
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

bool DecodeFile(const std::wstring& path, std::vector<std::uint8_t>& rgba, unsigned& width, unsigned& height) {
    UINT w = 0;
    UINT h = 0;
    if (!Decode(path, rgba, w, h)) return false;
    width = w;
    height = h;
    return true;
}

const Texture* Get(const std::wstring& path, const std::wstring& cacheKey) {
    Entry& e = g_cache[cacheKey.empty() ? path : cacheKey];
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

// --- In the background (covers: dozens at once) -----------------------------------
// The picture is decoded (and made smaller if it's big) and sent to the GPU by
// two helper threads, so the screen never waits; until then it's nullptr.
// (Direct3D 11 lets other threads create textures.)
namespace {

struct AsyncEntry {
    std::atomic<int> state{0};  // 0 waiting, 1 ready, 2 failed
    Texture texture;
    ID3D11ShaderResourceView* view = nullptr;
};
std::map<std::wstring, std::shared_ptr<AsyncEntry>> g_async;  // (UI thread)
std::mutex g_asyncMutex;
std::condition_variable g_asyncWake;
std::deque<std::pair<std::wstring, std::shared_ptr<AsyncEntry>>> g_asyncQueue;
std::vector<std::thread> g_asyncThreads;
std::atomic<bool> g_asyncStop{false};

void AsyncWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::pair<std::wstring, std::shared_ptr<AsyncEntry>> job;
        {
            std::unique_lock lock(g_asyncMutex);
            g_asyncWake.wait(lock, [] { return g_asyncStop.load() || !g_asyncQueue.empty(); });
            if (g_asyncStop.load()) break;
            job = std::move(g_asyncQueue.front());
            g_asyncQueue.pop_front();
        }
        std::vector<std::uint8_t> rgba;
        UINT w = 0, h = 0;
        AsyncEntry& e = *job.second;
        if (g_device == nullptr || !Decode(job.first, rgba, w, h)) {
            e.state.store(2);
            continue;
        }
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
        e.state.store(e.view != nullptr ? 1 : 2);
    }
    CoUninitialize();
}

}  // namespace

const Texture* GetAsync(const std::wstring& path) {
    auto it = g_async.find(path);
    if (it == g_async.end()) {
        auto e = std::make_shared<AsyncEntry>();
        g_async[path] = e;
        {
            std::lock_guard lock(g_asyncMutex);
            if (g_asyncThreads.empty()) {
                for (int i = 0; i < 2; ++i) g_asyncThreads.emplace_back(AsyncWorker);
            }
            g_asyncQueue.emplace_back(path, e);
        }
        g_asyncWake.notify_one();
        return nullptr;
    }
    return it->second->state.load() == 1 ? &it->second->texture : nullptr;
}

void ReleaseAll() {
    g_asyncStop.store(true);
    g_asyncWake.notify_all();
    for (auto& t : g_asyncThreads) {
        if (t.joinable()) t.join();
    }
    g_asyncThreads.clear();
    for (auto& [path, e] : g_async) {
        if (e->view != nullptr) e->view->Release();
    }
    g_async.clear();
    for (auto& [path, e] : g_cache) {
        if (e.view != nullptr) e.view->Release();
    }
    g_cache.clear();
}

}  // namespace luma::launcher::gui::textures
