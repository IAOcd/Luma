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
#include <string>
#include <cwchar>
#include <intrin.h>
#include <d3d11.h>
#include <dxgi1_5.h>  // 1_5: to read the tearing flag in the diagnostic log
#include <wrl/client.h>  // ComPtr: libera objetos COM automáticamente

#include <atomic>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/frame_stats.h"
#include "perf/gpu_timer.h"
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

// --- Diagnostics -------------------------------------------------------------------
bool g_loggedPresent = false;

// Once per session: what the game asks for, so we know where a cap comes from.
void LogPresentOnce(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    if (g_loggedPresent) {
        return;
    }
    g_loggedPresent = true;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain1;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1)))) {
        swapchain1->GetDesc1(&desc);
    }
    BOOL fullscreen = FALSE;
    swapchain->GetFullscreenState(&fullscreen, nullptr);
    log::Info("Present: the game asks for SyncInterval {} (flags 0x{:X}); swapchain {}x{}, {} buffers, swap effect "
              "{}, flags 0x{:X} (tearing {}), {}.",
              syncInterval, flags, desc.Width, desc.Height, desc.BufferCount, static_cast<int>(desc.SwapEffect),
              desc.Flags, (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? "allowed" : "not allowed",
              fullscreen ? "exclusive fullscreen" : "windowed/borderless");
}

// "thread_local" = cada hilo tiene su propia copia de esta variable.
// Internamente, Present de Windows podría llamar a Present1 (o al revés).
// Con este contador detectamos esa llamada "anidada" y no dibujamos dos veces.
// --- V-Sync apagado de verdad (módulo "V-Sync off") ---------------------------------
//  El juego ya pide "no esperar al monitor" (SyncInterval 0, con gfx_vsync:0
//  en options.txt), pero crea su swapchain SIN el permiso de "tearing"
//  (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING). Sin ese permiso, en ventana o en
//  pantalla completa sin bordes, Windows igual frena los FPS (Mojang lo tiene
//  reportado: MCPE-166745). El permiso solo se puede dar AL CREAR el
//  swapchain (Microsoft no deja agregarlo después), así que:
//    1. Enganchamos la creación (IDXGIFactory2::CreateSwapChainForHwnd) y le
//       agregamos el permiso, si la placa de video y Windows lo soportan. Por
//       sí solo no cambia nada: solo PERMITE presentar sin esperar.
//    2. ResizeBuffers: Windows exige que el permiso siga igual al cambiar el
//       tamaño de la ventana; se lo volvemos a poner.
//    3. Present: con el módulo activo, intervalo 0 y la marca
//       DXGI_PRESENT_ALLOW_TEARING (la que de verdad saca el límite).
//  Para que el paso 1 llegue a tiempo, Luma tiene que cargarse ANTES de que
//  el juego cree su swapchain: el launcher la inyecta apenas aparece el juego.
//  (La idea es la de Igneous, de Aetopia, y la opción "vsync" de Flarial,
//  estudiadas; el código es propio.)
constexpr int kCreateSwapChainForHwndIndex = 15;  // en la vtable de IDXGIFactory2
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
bool g_tearingSupported = false;              // ¿la placa de video y Windows lo permiten?
std::atomic<bool> g_uncapped{false};          // el módulo está activo
std::atomic<bool> g_sawGameSwapchain{false};  // ¿vimos al juego crear su swapchain?
std::atomic<bool> g_gameHasTearing{false};    // ¿quedó con el permiso?
// El último swapchain visto en Present y si tiene el permiso (hilo de render).
// Cuándo creó el juego su última swapchain (GetTickCount64; 0 = ninguna vista).
std::atomic<ULONGLONG> g_lastCreate{0};
constexpr ULONGLONG kSettleMs = 8000;  // (al arrancar, el juego crea la última unos 6 s después de la primera)
IDXGISwapChain* g_lastSwapchain = nullptr;
bool g_lastHasTearing = false;

bool HasTearing(IDXGISwapChain* swapchain) {
    if (swapchain == g_lastSwapchain) return g_lastHasTearing;
    g_lastSwapchain = swapchain;
    g_lastHasTearing = false;
    ComPtr<IDXGISwapChain1> swapchain1;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) && SUCCEEDED(swapchain1->GetDesc1(&desc))) {
        g_lastHasTearing = (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }
    return g_lastHasTearing;
}

// Con el módulo activo: no esperar al monitor y, si se puede, con tearing.
void Uncap(IDXGISwapChain* swapchain, UINT& syncInterval, UINT& flags) {
    if (!g_uncapped.load(std::memory_order_relaxed) || (flags & DXGI_PRESENT_TEST)) return;
    syncInterval = 0;
    if (HasTearing(swapchain)) flags |= DXGI_PRESENT_ALLOW_TEARING;
}

// Si el swapchain se creó con el permiso, al cambiar de tamaño tiene que seguir.
UINT KeepTearing(IDXGISwapChain* swapchain, UINT flags) {
    if (HasTearing(swapchain)) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    return flags;
}

// --- La cola de cada swapchain ----------------------------------------------------------
// En DirectX 12, las imágenes de una swapchain SOLO se pueden tocar desde la
// cola de comandos con la que se creó (el "device" de CreateSwapChainForHwnd).
// Usar otra = Windows da el dispositivo por perdido (DEVICE_REMOVED) y el juego
// se pasa a DirectX 11. Con Luma cargada temprano, el juego tiene varias colas
// de dibujo mientras arranca: por eso anotamos cuál usó para cada swapchain.
struct SwapchainQueue {
    IDXGISwapChain* swapchain = nullptr;
    ID3D12CommandQueue* queue = nullptr;  // (con AddRef: no se destruye mientras la guardamos)
};
SRWLOCK g_queuesLock = SRWLOCK_INIT;
SwapchainQueue g_queues[4];
int g_queuesNext = 0;

void RememberQueue(IDXGISwapChain1* swapchain, ID3D12CommandQueue* queue) {
    if (swapchain == nullptr || queue == nullptr) return;
    AcquireSRWLockExclusive(&g_queuesLock);
    SwapchainQueue& slot = g_queues[g_queuesNext];
    g_queuesNext = (g_queuesNext + 1) % 4;
    if (slot.queue != nullptr) slot.queue->Release();
    queue->AddRef();
    slot.swapchain = swapchain;
    slot.queue = queue;
    ReleaseSRWLockExclusive(&g_queuesLock);
}

// Describe una swapchain para el log (diagnóstico: qué crea el proceso y cuándo).
void LogCreate(const char* what, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc, HRESULT hr,
               void* caller) {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IUnknown> d3d11;
    const char* kind = "?";
    if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        kind = "DirectX 12";
    } else if (device != nullptr &&
               SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(d3d11.GetAddressOf())))) {
        kind = "DirectX 11";
    }
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(caller), &module);
    if (module != nullptr) GetModuleFileNameW(module, name, MAX_PATH);
    const wchar_t* shortName = std::wcsrchr(name, L'\\');
    std::string moduleName;
    for (const wchar_t* c = shortName ? shortName + 1 : name; *c != 0; ++c) {
        moduleName += *c < 128 ? static_cast<char>(*c) : '?';
    }
    log::Info("Swapchain {}: {} {}x{}, flags 0x{:X}, efecto {}, ventana 0x{:X} ({}), resultado 0x{:08X}, pedida desde "
              "{}+0x{:X}.",
              what, kind, desc ? desc->Width : 0, desc ? desc->Height : 0, desc ? desc->Flags : 0,
              desc ? static_cast<int>(desc->SwapEffect) : -1, reinterpret_cast<std::uintptr_t>(window),
              window == overlay::GameWindowForOverlay() ? "la del overlay" : "otra", static_cast<unsigned>(hr),
              moduleName.empty() ? std::string("?") : moduleName,
              module ? reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(module) : 0);
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndDetour(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                       IDXGIOutput* output, IDXGISwapChain1** swapchain) {
    hooks::ActiveCallGuard guard;
    void* caller = _ReturnAddress();
    const bool unloading = g_unloading.load(std::memory_order_acquire);
    if (!unloading) {
        // Si el overlay usa una swapchain EN ESTA VENTANA, soltarla antes (sea
        // del tipo que sea la nueva): si no, Windows no deja crear otra
        // ("acceso denegado") y la pantalla queda negra.
        overlay::BeforeSwapchainReplaced(window);
        g_lastCreate.store(GetTickCount64());
        g_lastSwapchain = nullptr;
    }
    ComPtr<ID3D12CommandQueue> queue;
    const bool dx12 = device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)));
    // El permiso de tearing: SIEMPRE (en swapchains de DirectX 12 "flip", las
    // del juego). Por sí solo no cambia nada: solo PERMITE presentar sin
    // esperar al monitor. Así el módulo V-Sync off funciona al instante, sin
    // reiniciar el juego (lo mismo hace Igneous, de Aetopia).
    const bool addTearing = !unloading && dx12 && desc != nullptr && g_tearingSupported &&
                            (desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                             desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    if (dx12) g_sawGameSwapchain.store(true);
    if (!addTearing) {
        const HRESULT hr = g_originalCreateSwapChainForHwnd(factory, device, window, desc, fullscreen, output, swapchain);
        LogCreate("creada", device, window, desc, hr, caller);
        if (SUCCEEDED(hr) && dx12 && swapchain != nullptr) RememberQueue(*swapchain, queue.Get());
        return hr;
    }
    DXGI_SWAP_CHAIN_DESC1 withTearing = *desc;
    withTearing.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    HRESULT hr = g_originalCreateSwapChainForHwnd(factory, device, window, &withTearing, fullscreen, output, swapchain);
    if (SUCCEEDED(hr)) {
        g_gameHasTearing.store(true);
        LogCreate("creada CON permiso de tearing", device, window, &withTearing, hr, caller);
        if (swapchain != nullptr) RememberQueue(*swapchain, queue.Get());
        return hr;
    }
    // Si con el permiso falla, como el juego lo pidió (nunca romper nada).
    const HRESULT plain = g_originalCreateSwapChainForHwnd(factory, device, window, desc, fullscreen, output, swapchain);
    log::Warn("V-Sync: con permiso de tearing la swapchain falló (0x{:08X}).", static_cast<unsigned>(hr));
    LogCreate("creada sin permiso (reintento)", device, window, desc, plain, caller);
    if (SUCCEEDED(plain) && swapchain != nullptr) RememberQueue(*swapchain, queue.Get());
    return plain;
}

// DIAGNÓSTICO: si un Present falla (por ejemplo "dispositivo perdido"), decir
// por qué (una vez por tipo de error). Así sabemos si algo hace que el juego
// abandone DirectX 12.
void LogPresentFailure(IDXGISwapChain* swapchain, HRESULT result) {
    static HRESULT logged[4]{};
    for (HRESULT& h : logged) {
        if (h == result) return;
        if (h == 0) {
            h = result;
            break;
        }
    }
    HRESULT reason = S_OK;
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) reason = device->GetDeviceRemovedReason();
    log::Warn("Present falló: 0x{:08X}; dispositivo perdido por: 0x{:08X}.", static_cast<unsigned>(result),
              static_cast<unsigned>(reason));
}

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
    // Closes this frame's GPU measurement (does nothing when it's off).
    perf::gputimer::OnPresent(g_originalExecuteCommandLists);
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
    if (t_presentDepth == 1) {
        LogPresentOnce(swapchain, syncInterval, flags);
    }
    HandlePresent(swapchain, flags);
    Uncap(swapchain, syncInterval, flags);
    const HRESULT result = g_originalPresent(swapchain, syncInterval, flags);
    if (FAILED(result)) LogPresentFailure(swapchain, result);
    --t_presentDepth;
    return result;
}

HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* swapchain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    hooks::ActiveCallGuard guard;
    ++t_presentDepth;
    if (t_presentDepth == 1) {
        LogPresentOnce(swapchain, syncInterval, flags);
    }
    HandlePresent(swapchain, flags);
    Uncap(swapchain, syncInterval, flags);
    const HRESULT result = g_originalPresent1(swapchain, syncInterval, flags, parameters);
    if (FAILED(result)) LogPresentFailure(swapchain, result);
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
    flags = KeepTearing(swapchain, flags);
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
    flags = KeepTearing(swapchain, flags);
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
        // With the GPU timer on, wraps the game's lists in two timestamps.
        // Off, it just calls the original (one atomic read).
        perf::gputimer::Execute(queue, count, lists, g_originalExecuteCommandLists);
        return;
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
    void* createSwapChainForHwnd = nullptr;
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
            out.createSwapChainForHwnd = VTableEntry(factory.Get(), kCreateSwapChainForHwndIndex);
            // ¿Se puede presentar con tearing? (Windows 10 1709+ y placa de video compatible)
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory.As(&factory5)) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
                g_tearingSupported = tearing == TRUE;
            }
            log::Info("V-Sync: tearing {} en esta PC.", g_tearingSupported ? "soportado" : "NO soportado");
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
    // (Si este falla, Luma funciona igual: solo el V-Sync off queda a medias.)
    if (addresses.createSwapChainForHwnd != nullptr &&
        !hooks::Create(addresses.createSwapChainForHwnd, reinterpret_cast<void*>(&CreateSwapChainForHwndDetour),
                       reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd),
                       "IDXGIFactory2::CreateSwapChainForHwnd")) {
        log::Warn("V-Sync: no se pudo enganchar la creación del swapchain.");
    }
    return ok;
}

ID3D12CommandQueue* QueueFor(IDXGISwapChain* swapchain) {
    ID3D12CommandQueue* found = nullptr;
    AcquireSRWLockShared(&g_queuesLock);
    for (const SwapchainQueue& slot : g_queues) {
        if (slot.swapchain == swapchain && slot.queue != nullptr) {
            found = slot.queue;
            found->AddRef();
            break;
        }
    }
    ReleaseSRWLockShared(&g_queuesLock);
    return found;
}

bool SwapchainSettled() {
    const ULONGLONG last = g_lastCreate.load(std::memory_order_relaxed);
    return last == 0 || GetTickCount64() - last >= kSettleMs;
}

void SetUncapped(bool on) {
    g_uncapped.store(on);
}

TearingState Tearing() {
    if (!g_tearingSupported) return TearingState::Unsupported;
    if (g_gameHasTearing.load()) return TearingState::Active;
    if (g_sawGameSwapchain.load()) return TearingState::Failed;
    return TearingState::TooLate;
}

void BeginUnload() {
    g_unloading.store(true, std::memory_order_release);
}

}  // namespace luma::render::dx12
