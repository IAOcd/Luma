// =============================================================================
//  dx12_hooks.cpp - Cómo encontramos las funciones de DirectX y las hookeamos
// =============================================================================
//  PROBLEMA: Present no es una función "normal" que se pueda buscar por nombre.
//  Es un método de un objeto COM (la swapchain). Los objetos COM guardan sus
//  métodos en una tabla de punteros llamada "vtable"; el orden de esa tabla
//  está fijado por Microsoft y NUNCA cambia (si cambiara, todos los programas
//  de Windows dejarían de funcionar).
//
//  SOLUCIÓN: creamos nosotros, por un instante, una swapchain de prueba sobre
//  una ventana invisible. Leemos de su vtable la dirección de Present y demás,
//  y la destruimos. Esa dirección es la misma que usa la swapchain del juego,
//  porque ambas son objetos de la misma clase de dxgi.dll.
//
//  Posiciones en la vtable (contando desde 0, según los headers de Windows):
//    IDXGISwapChain:    8 = Present, 13 = ResizeBuffers
//    IDXGISwapChain1:  22 = Present1
//    IDXGISwapChain3:  39 = ResizeBuffers1
//    ID3D12CommandQueue: 10 = ExecuteCommandLists
// =============================================================================
#include "render/dx12_hooks.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>  // ComPtr: libera objetos COM automáticamente

#include <atomic>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/overlay.h"

using Microsoft::WRL::ComPtr;

namespace luma::render::dx12 {

namespace {

constexpr int kPresentIndex = 8;
constexpr int kResizeBuffersIndex = 13;
constexpr int kPresent1Index = 22;
constexpr int kResizeBuffers1Index = 39;
constexpr int kExecuteCommandListsIndex = 10;

// --- Tipos de las funciones originales -----------------------------------------
// Describen qué parámetros recibe cada función, para poder llamarlas.
// STDMETHODCALLTYPE es la "convención de llamada" de COM.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                                    UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
                                                     DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);

// Trampolines: MinHook los rellena; llamarlos ejecuta la función original.
PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
ResizeBuffers1Fn g_originalResizeBuffers1 = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

std::atomic<bool> g_unloading{false};

// "thread_local" = cada hilo tiene su propia copia de esta variable.
// Internamente, Present de Windows podría llamar a Present1 (o al revés).
// Con este contador detectamos esa llamada "anidada" y no dibujamos dos veces.
thread_local int t_presentDepth = 0;

// --- Detours --------------------------------------------------------------------

// Lo que hacemos en cada frame, común a Present y Present1.
void HandlePresent(IDXGISwapChain* swapchain, UINT flags) {
    // Solo en la llamada "de afuera" (no en una anidada), y nunca si el juego
    // pasó DXGI_PRESENT_TEST: eso significa "¿se podría mostrar?", sin mostrar
    // nada, así que no es un frame real.
    if (t_presentDepth != 1 || (flags & DXGI_PRESENT_TEST) ||
        g_unloading.load(std::memory_order_acquire)) {
        return;
    }
    // Medimos cuánto tarda TODO nuestro trabajo de este frame. (El propio
    // RecordFrame queda fuera de la medición; cuesta unas decenas de
    // nanosegundos.)
    const std::int64_t start = perf::Now();
    const bool initialized = overlay::OnPresent(swapchain);
    const std::int64_t end = perf::Now();
    perf::RecordFrame(start, end, overlay::IsMenuOpen(), initialized);
    if (initialized) {
        // Pasa una sola vez, así que escribir en el log aquí no importa.
        log::Info("Inicialización del overlay: {:.2f} ms (una sola vez, no cuenta como costo por "
                  "frame).", perf::TicksToMs(end - start));
    }
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    hooks::ActiveCallGuard guard;
    ++t_presentDepth;
    HandlePresent(swapchain, flags);
    const HRESULT result = g_originalPresent(swapchain, syncInterval, flags);
    --t_presentDepth;
    return result;
}

HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* swapchain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    hooks::ActiveCallGuard guard;
    ++t_presentDepth;
    HandlePresent(swapchain, flags);
    const HRESULT result = g_originalPresent1(swapchain, syncInterval, flags, parameters);
    --t_presentDepth;
    return result;
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* swapchain, UINT bufferCount,
                                              UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags) {
    hooks::ActiveCallGuard guard;
    const bool active = !g_unloading.load(std::memory_order_acquire);
    if (active) {
        overlay::BeforeResize(swapchain);  // soltar nuestras referencias a los buffers
    }
    const HRESULT result =
        g_originalResizeBuffers(swapchain, bufferCount, width, height, format, flags);
    if (active) {
        overlay::AfterResize(swapchain, result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE ResizeBuffers1Detour(IDXGISwapChain3* swapchain, UINT bufferCount,
                                               UINT width, UINT height, DXGI_FORMAT format,
                                               UINT flags, const UINT* nodeMask,
                                               IUnknown* const* presentQueues) {
    hooks::ActiveCallGuard guard;
    const bool active = !g_unloading.load(std::memory_order_acquire);
    if (active) {
        overlay::BeforeResize(swapchain);
    }
    const HRESULT result = g_originalResizeBuffers1(swapchain, bufferCount, width, height, format,
                                                    flags, nodeMask, presentQueues);
    if (active) {
        overlay::AfterResize(swapchain, result);
    }
    return result;
}

void STDMETHODCALLTYPE ExecuteCommandListsDetour(ID3D12CommandQueue* queue, UINT count,
                                                 ID3D12CommandList* const* lists) {
    hooks::ActiveCallGuard guard;
    if (!g_unloading.load(std::memory_order_acquire)) {
        // Esto se llama muchas veces por frame. Una vez que la cola está
        // capturada, CaptureQueue solo hace una comparación y vuelve.
        overlay::CaptureQueue(queue);
    }
    g_originalExecuteCommandLists(queue, count, lists);
}

// --- Swapchain de prueba ----------------------------------------------------------

// Direcciones leídas de las vtables.
struct VTableAddresses {
    void* present = nullptr;
    void* present1 = nullptr;
    void* resizeBuffers = nullptr;
    void* resizeBuffers1 = nullptr;
    void* executeCommandLists = nullptr;
};

// Lee la entrada número "index" de la vtable de un objeto COM.
// Un objeto COM empieza con un puntero a su vtable, que es un arreglo de
// punteros a funciones.
void* VTableEntry(void* object, int index) {
    void** vtable = *static_cast<void***>(object);
    return vtable[index];
}

bool ReadVTables(VTableAddresses& out) {
    // 1. Ventana invisible: una swapchain necesita una ventana sobre la cual
    //    dibujar. Nunca la mostramos.
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"LumaDummyWindow";
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100,
                                  100, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        UnregisterClassW(windowClass.lpszClassName, instance);
        log::Error("No se pudo crear la ventana de prueba (error {}).", GetLastError());
        return false;
    }

    bool ok = false;
    // Este bloque { } existe para que los ComPtr se liberen ANTES de destruir
    // la ventana (al salir de las llaves).
    {
        ComPtr<IDXGIFactory2> factory;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGISwapChain1> swapchain;

        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) {
            hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        }
        if (SUCCEEDED(hr)) {
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
        }
        if (SUCCEEDED(hr)) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 100;
            desc.Height = 100;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // el modelo que exige DX12
            hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr,
                                                 &swapchain);
        }
        if (SUCCEEDED(hr)) {
            out.present = VTableEntry(swapchain.Get(), kPresentIndex);
            out.resizeBuffers = VTableEntry(swapchain.Get(), kResizeBuffersIndex);
            out.present1 = VTableEntry(swapchain.Get(), kPresent1Index);
            out.resizeBuffers1 = VTableEntry(swapchain.Get(), kResizeBuffers1Index);
            out.executeCommandLists = VTableEntry(queue.Get(), kExecuteCommandListsIndex);
            ok = true;
        } else {
            log::Error("No se pudo crear la swapchain DX12 de prueba (HRESULT 0x{:08X}).",
                       static_cast<unsigned>(hr));
        }
    }

    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    return ok;
}

}  // namespace

bool CreateHooks() {
    VTableAddresses addresses;
    if (!ReadVTables(addresses)) {
        return false;
    }

    // reinterpret_cast<void**>(&g_originalPresent): MinHook escribe la
    // dirección del trampolín directamente en nuestra variable.
    bool ok = true;
    ok &= hooks::Create(addresses.present, reinterpret_cast<void*>(&PresentDetour),
                        reinterpret_cast<void**>(&g_originalPresent), "IDXGISwapChain::Present");
    ok &= hooks::Create(addresses.present1, reinterpret_cast<void*>(&Present1Detour),
                        reinterpret_cast<void**>(&g_originalPresent1), "IDXGISwapChain1::Present1");
    ok &= hooks::Create(addresses.resizeBuffers, reinterpret_cast<void*>(&ResizeBuffersDetour),
                        reinterpret_cast<void**>(&g_originalResizeBuffers),
                        "IDXGISwapChain::ResizeBuffers");
    ok &= hooks::Create(addresses.resizeBuffers1, reinterpret_cast<void*>(&ResizeBuffers1Detour),
                        reinterpret_cast<void**>(&g_originalResizeBuffers1),
                        "IDXGISwapChain3::ResizeBuffers1");
    ok &= hooks::Create(addresses.executeCommandLists,
                        reinterpret_cast<void*>(&ExecuteCommandListsDetour),
                        reinterpret_cast<void**>(&g_originalExecuteCommandLists),
                        "ID3D12CommandQueue::ExecuteCommandLists");
    return ok;
}

void BeginUnload() {
    g_unloading.store(true, std::memory_order_release);
}

}  // namespace luma::render::dx12
