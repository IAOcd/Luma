// =============================================================================
//  overlay.cpp - Dibujar ImGui encima del juego con DirectX 12 (u 11)
// =============================================================================
//  DIRECTX 11 Y 12: el juego usa DirectX 12, pero otros clients (Flarial con
//  "Better Frames") lo pasan a DirectX 11, a veces en plena partida. El
//  overlay sabe dibujar con los dos: abajo, lo que dice "DX12" es el camino de
//  DirectX 12, y DirectX 11 es más simple (no hay command lists ni colas: se
//  dibuja directo con el "contexto" del juego). Si el juego cambia de uno a
//  otro, soltamos todo lo de la GPU vieja y lo creamos en la nueva; ImGui, el
//  menú y las texturas de Luma (render/gpu.h) siguen igual.
//
//  Cómo funciona un frame con el menú abierto:
//
//    1. El juego termina de dibujar su imagen en un "back buffer" (una imagen
//       en la memoria de la GPU) y llama a Present para mostrarla.
//    2. Nuestro hook de Present corre ANTES del Present original:
//         a. ImGui calcula qué dibujar (el menú) como una lista de triángulos.
//         b. Grabamos una "command list": instrucciones para la GPU que dicen
//            "dibuja estos triángulos encima del back buffer".
//         c. La mandamos a la cola de comandos del juego, DETRÁS de lo que
//            el juego ya mandó. La GPU dibuja el menú encima del frame.
//    3. Se ejecuta el Present original y la pantalla muestra todo junto.
//
//  Con el menú CERRADO, OnPresent sale casi de inmediato (unas pocas lecturas
//  de variables). Así Luma no cuesta nada cuando no hay nada que dibujar.
//
//  HILOS: Present corre en el hilo de render del juego y los mensajes de la
//  ventana en otro hilo. Ambos tocan ImGui, que NO está pensado para usarse
//  desde dos hilos a la vez, así que todo acceso pasa por luma::StateMutex().
//  Regla para evitar bloqueos: nunca tenerlo tomado mientras se llama a una
//  función original del juego o de DirectX que podría mandar mensajes a la
//  ventana.
// =============================================================================
#include "render/overlay.h"

#include "render/dx12_hooks.h"
#include "sdk/paperdoll.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "core/config.h"
#include "core/logger.h"
#include "core/paths.h"
#include "core/state.h"
#include "modules/module_manager.h"
#include "perf/benchmark.h"
#include "perf/gpu_timer.h"
#include "render/assets.h"
#include "render/cinematic.h"
#include "render/gpu.h"
#include "render/hud.h"
#include "render/hud_editor.h"
#include "render/world_map.h"
#include "render/icons.h"
#include "render/input.h"
#include "render/item_icons.h"
#include "render/loading.h"
#include "render/font_fallback.h"
#include "render/menu.h"
#include "sdk/camera.h"
#include "render/music.h"
#include "render/notifications.h"
#include "render/postfx.h"
#include "render/theme.h"
#include "sdk/mouse_grab.h"

// El backend de Win32 declara esta función "comentada" en su header para que
// cada programa la declare por su cuenta. La declaramos aquí.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;

namespace luma::render::overlay {

namespace {

// Una swapchain puede tener como máximo 16 buffers (límite de DXGI).
constexpr UINT kMaxBuffers = DXGI_MAX_SWAP_CHAIN_BUFFERS;

// Estado de cada back buffer de la swapchain.
struct FrameContext {
    // Memoria donde la GPU guarda los comandos de la command list. Hay una
    // por buffer, porque no se puede reutilizar mientras la GPU la lee.
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> backBuffer;   // la imagen del frame
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};   // "vista" para dibujar sobre ella
    UINT64 fenceValue = 0;               // ver WaitForFrame
};

enum class State {
    Waiting,      // Todavía no vimos un frame con la cola capturada.
    Ready,        // Todo listo para dibujar.
    Unsupported,  // El juego no usa ni DX12 ni DX11: overlay desactivado.
    Failed,       // Algo falló al crear recursos: overlay desactivado.
};

// Con qué versión de DirectX dibuja el juego ahora.
enum class Api { None, Dx12, Dx11 };

const char* ApiName(Api api) {
    switch (api) {
        case Api::Dx12: return "DirectX 12";
        case Api::Dx11: return "DirectX 11";
        default:        return "ninguno";
    }
}

// Todo el estado de dibujo en un solo lugar.
struct Renderer {
    Api api = Api::None;
    IDXGISwapChain* swapchain = nullptr;  // solo para identificarla; no la "poseemos"
    HWND window = nullptr;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // Un "fence" es un contador que la GPU incrementa al terminar trabajo.
    // Sirve para saber si la GPU ya terminó de usar algo antes de reutilizarlo.
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceCounter = 0;

    std::array<FrameContext, kMaxBuffers> frames{};
    UINT bufferCount = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;

    // DirectX 11: el dispositivo, su contexto (por donde se le dan órdenes a
    // la GPU) y la "vista" para dibujar sobre la imagen del frame.
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11RenderTargetView> rtv11;

    bool imguiContext = false;
    bool win32Backend = false;
    bool dx12Backend = false;
    bool dx11Backend = false;
    bool resizing = false;
    bool replaced = false;  // el juego está creando otra swapchain: soltamos la vieja (ver BeforeSwapchainReplaced)
    bool loggedOtherSwapchain = false;
};

Renderer g_r;
// El candado de todo el estado de Luma (ImGui incluido) es luma::StateMutex(),
// ver core/state.h.

std::atomic<State> g_state{State::Waiting};
std::atomic<bool> g_menuOpen{false};
// ¿ImGui tiene una caja de texto activa? (Se actualiza cada frame con el menú
// abierto; la WndProc lo consulta para no cerrar el menú al escribir una L.)
std::atomic<bool> g_wantsTextInput{false};
// La cola de comandos del juego (capturada en ExecuteCommandLists).
std::atomic<ID3D12CommandQueue*> g_queue{nullptr};

const char* FormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:     return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:  return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT (HDR)";
        default:                             return "otro";
    }
}

// --- Sincronización con la GPU ------------------------------------------------

// Espera hasta que la GPU haya llegado al valor "value" del fence.
void WaitForFenceValue(UINT64 value) {
    if (g_r.fence->GetCompletedValue() < value) {
        g_r.fence->SetEventOnCompletion(value, g_r.fenceEvent);
        WaitForSingleObject(g_r.fenceEvent, 2000);  // máximo 2 s, por seguridad
    }
}

// Espera a que la GPU termine TODO lo que le mandamos hasta ahora.
void WaitForGpu() {
    ID3D12CommandQueue* queue = g_queue.load();
    if (!g_r.fence || queue == nullptr) {
        return;
    }
    const UINT64 value = ++g_r.fenceCounter;
    if (SUCCEEDED(queue->Signal(g_r.fence.Get(), value))) {
        WaitForFenceValue(value);
    }
}

// --- Recursos de DirectX ---------------------------------------------------------
// "Device resources" = todo lo que depende de los buffers de la swapchain y
// hay que recrear cuando cambia su tamaño.

void DestroyDeviceResources() {
    WaitForGpu();  // Nunca liberar algo que la GPU todavía podría estar usando.
    if (g_r.dx12Backend) {
        ImGui_ImplDX12_Shutdown();
        g_r.dx12Backend = false;
    }
    if (g_r.dx11Backend) {
        ImGui_ImplDX11_Shutdown();
        g_r.dx11Backend = false;
    }
    g_r.rtv11.Reset();
    for (FrameContext& frame : g_r.frames) {
        frame = FrameContext{};  // libera allocator y back buffer (ComPtr)
    }
    gpu::DestroyBlurTargets();  // dependen del tamaño de la pantalla
    g_r.commandList.Reset();
    g_r.rtvHeap.Reset();
    g_r.bufferCount = 0;
}

bool CreateDx12Resources(IDXGISwapChain3* swapchain) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(swapchain->GetDesc1(&desc)) || desc.BufferCount == 0 ||
        desc.BufferCount > kMaxBuffers) {
        log::Error("No se pudo leer la descripción de la swapchain.");
        return false;
    }
    g_r.bufferCount = desc.BufferCount;
    g_r.format = desc.Format;
    g_r.width = desc.Width;
    g_r.height = desc.Height;

    ID3D12Device* device = g_r.device.Get();

    // "Descriptor heaps": tablas donde DX12 guarda descripciones de recursos.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = g_r.bufferCount;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_r.rtvHeap)))) {
        log::Error("No se pudo crear el heap de RTV.");
        return false;
    }

    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_r.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_r.bufferCount; ++i) {
        FrameContext& frame = g_r.frames[i];
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&frame.allocator))) ||
            FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&frame.backBuffer)))) {
            log::Error("No se pudieron preparar los buffers de la swapchain.");
            return false;
        }
        frame.rtv = rtvHandle;
        device->CreateRenderTargetView(frame.backBuffer.Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvSize;
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         g_r.frames[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&g_r.commandList)))) {
        log::Error("No se pudo crear la command list.");
        return false;
    }
    g_r.commandList->Close();  // Se crea "abierta"; la cerramos hasta usarla.

    // La fuente de ImGui va en el lugar 0 de la tabla de descriptores de Luma
    // (render/gpu.h), que dura toda la sesión.
    if (!ImGui_ImplDX12_Init(device, static_cast<int>(g_r.bufferCount), g_r.format, gpu::SrvHeap(),
                             gpu::CpuSlot(gpu::kFontSlot), gpu::GpuSlot(gpu::kFontSlot))) {
        log::Error("No se pudo iniciar el backend DX12 de ImGui.");
        return false;
    }
    g_r.dx12Backend = true;
    // Compilar ya los shaders y crear la textura de la fuente (si no, ImGui lo
    // hace en el primer frame con el menú abierto y ese frame daría un tirón).
    ImGui_ImplDX12_CreateDeviceObjects();

    // Texturas del desenfoque del vidrio (dependen del tamaño de pantalla).
    gpu::CreateBlurTargets(g_r.width, g_r.height, g_r.format);
    return true;
}

// DirectX 11: mucho más simple. Una sola "vista" sobre el buffer 0: en DX11,
// con las swapchains modernas ("flip"), el buffer 0 siempre es el que se va a
// mostrar en este frame (Windows los rota por dentro).
bool CreateDx11Resources(IDXGISwapChain1* swapchain) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(swapchain->GetDesc1(&desc))) {
        log::Error("No se pudo leer la descripción de la swapchain (DX11).");
        return false;
    }
    g_r.bufferCount = desc.BufferCount;
    g_r.format = desc.Format;
    g_r.width = desc.Width;
    g_r.height = desc.Height;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(g_r.device11->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_r.rtv11))) {
        log::Error("No se pudo preparar la imagen de la swapchain (DX11).");
        return false;
    }
    if (!ImGui_ImplDX11_Init(g_r.device11.Get(), g_r.context11.Get())) {
        log::Error("No se pudo iniciar el backend DX11 de ImGui.");
        return false;
    }
    g_r.dx11Backend = true;
    ImGui_ImplDX11_CreateDeviceObjects();  // (igual que en DX12: no dar un tirón en el primer frame)
    return true;
}

// Lo de arriba, según el DirectX que use el juego ahora.
bool CreateDeviceResources(IDXGISwapChain* swapchain) {
    if (g_r.api == Api::Dx11) {
        ComPtr<IDXGISwapChain1> swapchain1;
        return SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) && CreateDx11Resources(swapchain1.Get());
    }
    ComPtr<IDXGISwapChain3> swapchain3;
    return g_r.api == Api::Dx12 && SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) &&
           CreateDx12Resources(swapchain3.Get());
}

// --- ImGui -----------------------------------------------------------------------

// Carga las fuentes Segoe UI de Windows (tienen tildes y ñ) al tamaño del
// tema: normal para el texto, más grande para los nombres de módulos y
// seminegrita para los títulos. Si no están, usa la de ImGui.
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    theme::Fonts& fonts = theme::GetFonts();
    fonts = theme::Fonts{};
    fontfallback::BeginBuild();  // the letters of other alphabets that songs needed (render/font_fallback.h)

    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    const std::filesystem::path fontsDir = std::filesystem::path(windowsDir) / L"Fonts";
    auto load = [&](const wchar_t* file, float size) -> ImFont* {
        std::error_code ec;
        const std::filesystem::path path = fontsDir / file;
        if (!std::filesystem::exists(path, ec)) {
            return nullptr;
        }
        ImFont* font = io.Fonts->AddFontFromFileTTF(paths::ToUtf8(path).c_str(), size);
        if (font != nullptr) fontfallback::MergeInto(io, size);  // + Japanese, Korean, Arabic... if needed
        return font;
    };

    const float base = theme::Get().fontSize * theme::UiScale();
    // La primera fuente agregada es la que ImGui usa por defecto.
    fonts.regular = load(L"segoeui.ttf", base);
    if (fonts.regular == nullptr) {
        fonts.regular = io.Fonts->AddFontDefault();
    }
    fonts.medium = load(L"segoeui.ttf", base * 1.22f);
    fonts.title = load(L"seguisb.ttf", base * 1.6f);  // Segoe UI Semibold
    if (fonts.medium == nullptr) {
        fonts.medium = fonts.regular;
    }
    if (fonts.title == nullptr) {
        fonts.title = fonts.medium;
    }
    // The lyrics are loaded at their real size (scaling a smaller font up
    // would look blurry).
    fonts.lyrics = load(L"segoeuib.ttf", base * 2.05f);
    if (fonts.lyrics == nullptr) {
        fonts.lyrics = fonts.title;
    }
    // Las del HUD (la pixelada del juego a varios tamaños exactos y la suave).
    hud::LoadFonts(io, theme::UiScale());
}

// El tamaño de letra cambió: regenerar la textura de la fuente en la GPU.
// Pasa solo cuando el usuario suelta el slider de "Tamaño de letra".
void RebuildFonts() {
    WaitForGpu();  // la textura vieja podría estar en uso
    LoadFonts();
    if (g_r.dx12Backend) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
        ImGui_ImplDX12_CreateDeviceObjects();
    } else if (g_r.dx11Backend) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
    theme::Apply();
    log::Info("Fuente regenerada ({:.0f} px).", theme::Get().fontSize);
}

void CreateImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_r.imguiContext = true;

    ImGuiIO& io = ImGui::GetIO();
    // ImGui por defecto guarda imgui.ini en la carpeta actual (la del juego).
    // No queremos escribir ahí; la config de Luma llega en la Fase 3.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // El juego esconde el cursor de Windows, así que ImGui dibuja el suyo.
    io.MouseDrawCursor = true;

    // Escala según la altura de la imagen: 1.0 en 1080p, 1.33 en 1440p, etc.
    theme::SetUiScale(std::clamp(static_cast<float>(g_r.height) / 1080.0f, 1.0f, 3.0f));

    LoadFonts();
    theme::Apply();

    ImGui_ImplWin32_Init(g_r.window);
    g_r.win32Backend = true;
}

// Con qué DirectX se creó esta swapchain.
Api ApiOf(IDXGISwapChain* swapchain) {
    ComPtr<ID3D12Device> device12;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device12)))) return Api::Dx12;
    ComPtr<ID3D11Device> device11;
    if (SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device11)))) return Api::Dx11;
    return Api::None;
}

enum class InitResult { Ok, Retry, Unsupported, Failed };

// Prepara lo que depende del DISPOSITIVO (la GPU y la versión de DirectX):
// fence, cola, texturas de Luma. No toca ImGui. Con g_mutex tomado.
InitResult InitDevice(IDXGISwapChain* swapchain) {
    const Api api = ApiOf(swapchain);
    if (api == Api::None) {
        return InitResult::Unsupported;
    }

    if (api == Api::Dx11) {
        // DirectX 11 no tiene colas ni fences: se dibuja con el "contexto
        // inmediato" del juego, desde este mismo hilo (el de Present).
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
            return InitResult::Failed;
        }
        device->GetImmediateContext(&context);
        if (!context || !gpu::InitDx11(device.Get(), context.Get())) {
            return InitResult::Failed;
        }
        g_r.device11 = device;
        g_r.context11 = context;
        g_r.api = Api::Dx11;
        return InitResult::Ok;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        return InitResult::Failed;
    }
    // Si vimos al juego CREAR esta swapchain, sabemos su cola exacta: es la
    // única con la que se pueden tocar sus imágenes (con otra, DirectX 12 da
    // el dispositivo por perdido). La usamos en lugar de la capturada.
    if (ID3D12CommandQueue* own = dx12::QueueFor(swapchain)) {
        ID3D12CommandQueue* previous = g_queue.exchange(own);  // (own ya trae su AddRef)
        if (previous != nullptr && previous != own) {
            previous->Release();
            log::Info("Overlay: se usa la cola con la que el juego creó la swapchain.");
        } else if (previous == own) {
            own->Release();  // (era la misma: una referencia de más)
        }
    }
    ID3D12CommandQueue* queue = g_queue.load();
    if (queue == nullptr) {
        return InitResult::Retry;  // todavía no sabemos qué cola usa el juego
    }
    // Comprobar que la cola capturada pertenece al mismo dispositivo que la
    // swapchain. Si no, la descartamos y esperamos a capturar otra.
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice != device) {
        log::Warn("La cola capturada no corresponde a la swapchain; se buscará otra.");
        if (g_queue.compare_exchange_strong(queue, nullptr)) {
            queue->Release();
        }
        return InitResult::Retry;
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_r.fence)))) {
        log::Error("No se pudo crear el fence de DX12.");
        return InitResult::Failed;
    }
    g_r.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Tabla de descriptores, subida de texturas y desenfoque (render/gpu.h).
    if (!gpu::Init(device.Get(), queue)) {
        return InitResult::Failed;
    }
    g_r.device = device;
    g_r.api = Api::Dx12;
    // The GPU timer measures this same queue (it only creates anything when
    // someone turns it on).
    perf::gputimer::Attach(device.Get(), queue);
    return InitResult::Ok;
}

// Suelta todo lo que depende del dispositivo (lo contrario de InitDevice),
// pero deja ImGui, el menú y las texturas de Luma (que vuelven a subirse).
void ReleaseDevice() {
    DestroyDeviceResources();
    perf::gputimer::Shutdown();  // (it waits for its own work on the GPU)
    gpu::ReleaseDevice();
    g_r.fence.Reset();
    if (g_r.fenceEvent != nullptr) {
        CloseHandle(g_r.fenceEvent);
        g_r.fenceEvent = nullptr;
    }
    g_r.device.Reset();
    g_r.context11.Reset();
    g_r.device11.Reset();
    g_r.api = Api::None;
    if (ID3D12CommandQueue* queue = g_queue.exchange(nullptr)) {
        queue->Release();
    }
}

// ¿Esta swapchain es del mismo dispositivo (y cola) que usamos ahora?
bool SameDevice(IDXGISwapChain* swapchain) {
    if (g_r.api == Api::Dx11) {
        ComPtr<ID3D11Device> device;
        return SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&device))) && device == g_r.device11;
    }
    if (g_r.api != Api::Dx12) {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device))) || device != g_r.device) {
        return false;
    }
    // Su cola tiene que ser la nuestra (si no, no podemos tocar sus imágenes).
    bool same = true;
    if (ID3D12CommandQueue* own = dx12::QueueFor(swapchain)) {
        same = own == g_queue.load();
        own->Release();
    }
    return same;
}

// La ventana de una swapchain (sirve para DX11 y DX12).
HWND WindowOf(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    return SUCCEEDED(swapchain->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
}

// Primera vez que vemos un frame listo: preparar todo. Con g_mutex tomado.
bool Initialize(IDXGISwapChain* swapchain) {
    switch (InitDevice(swapchain)) {
        case InitResult::Ok:
            break;
        case InitResult::Retry:
            return false;  // (seguimos esperando; se reintenta en el próximo frame)
        case InitResult::Unsupported:
            log::Warn("El juego no está usando DirectX 12 ni 11. "
                      "El overlay queda desactivado; el resto de Luma sigue funcionando.");
            g_state.store(State::Unsupported);
            return false;
        case InitResult::Failed:
            ReleaseDevice();
            g_state.store(State::Failed);
            return false;
    }

    g_r.swapchain = swapchain;
    g_r.window = WindowOf(swapchain);

    // The icons are drawn from one shared texture (render/icons.h): tell them
    // how to make and update textures (Luma's numbers work on DX11 and DX12).
    icons::SetBackend(
        [](const std::uint8_t* rgba, unsigned w, unsigned h) { return gpu::CreateTexture(rgba, w, h); },
        [](ImTextureID id, const std::uint8_t* rgba, unsigned w, unsigned h) { return gpu::UpdateTexture(id, rgba, w, h); });

    // Necesitamos el alto de la imagen antes de crear ImGui (para la escala).
    ComPtr<IDXGISwapChain1> swapchain1;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1)))) {
        swapchain1->GetDesc1(&desc);
    }
    g_r.height = desc.Height;

    CreateImGui();
    if (!CreateDeviceResources(swapchain)) {
        g_state.store(State::Failed);
        return false;
    }

    input::Install(g_r.window);

    log::Info("Overlay listo: {}, {}x{}, formato {}, {} buffers.", ApiName(g_r.api), g_r.width, g_r.height,
              FormatName(g_r.format), g_r.bufferCount);
    log::Info("Pulsa L para abrir el menú.");
    g_state.store(State::Ready);
    return true;
}

// El juego creó otra swapchain en nuestra ventana (y ya soltamos la vieja,
// ver BeforeSwapchainReplaced). Si es del mismo dispositivo, la adoptamos
// como tras un cambio de tamaño; si cambió la GPU o la versión de DirectX
// (Flarial "Better Frames": de 12 a 11), rehacemos lo del dispositivo.
// Con g_mutex tomado.
bool AdoptSwapchain(IDXGISwapChain* swapchain) {
    const Api before = g_r.api;
    if (before != Api::None && !SameDevice(swapchain)) {
        ReleaseDevice();
    }
    if (g_r.api == Api::None) {
        const InitResult result = InitDevice(swapchain);
        if (result != InitResult::Ok) {
            if (result == InitResult::Failed) ReleaseDevice();
            return false;  // (Retry: en el próximo frame, cuando sepamos su cola)
        }
        log::Info("Overlay: el juego pasó de {} a {}; Luma sigue dibujando.", ApiName(before), ApiName(g_r.api));
    }
    if (!CreateDeviceResources(swapchain)) {
        return false;
    }
    g_r.swapchain = swapchain;
    g_r.window = WindowOf(swapchain);
    g_r.replaced = false;
    g_r.loggedOtherSwapchain = false;
    log::Info("Overlay: el juego cambió de swapchain; seguimos en la nueva ({}, {}x{}).", ApiName(g_r.api), g_r.width,
              g_r.height);
    return true;
}

// DirectX 11: dibujar ImGui directo con el contexto del juego. Guardamos y
// devolvemos a dónde estaba dibujando el juego (ImGui ya guarda y devuelve
// el resto del estado: shaders, viewport, etc.).
void SubmitDx11(ImDrawData* drawData) {
    ID3D11DeviceContext* context = g_r.context11.Get();
    ID3D11RenderTargetView* oldTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDepth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, &oldDepth);

    ID3D11RenderTargetView* target = g_r.rtv11.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(drawData);

    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, oldDepth);
    for (ID3D11RenderTargetView* old : oldTargets) {
        if (old != nullptr) old->Release();  // (OMGetRenderTargets les sumó una referencia)
    }
    if (oldDepth != nullptr) oldDepth->Release();
}

// Dibuja un frame de ImGui sobre el back buffer actual. Con g_mutex tomado.
void RenderFrame(IDXGISwapChain* swapchain) {
    if (!g_r.dx12Backend && !g_r.dx11Backend) {
        return;  // En medio de un cambio de tamaño (o de DirectX).
    }
    ComPtr<IDXGISwapChain3> swapchain3;
    if (g_r.dx12Backend && FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3)))) {
        return;
    }

    if (theme::ConsumeFontRebuildRequest()) {
        RebuildFonts();
    }
    assets::UploadPending();  // el logo, la primera vez que está listo
    icons_items::UploadPending();  // íconos de ítems ya decodificados

    // Con el menú cerrado, ImGui no debe reaccionar al mouse (el HUD no se
    // toca) ni cambiar el cursor de Windows (el juego lo tiene escondido).
    const bool menuOpen = g_menuOpen.load(std::memory_order_relaxed);
    ImGuiIO& io = ImGui::GetIO();
    // Si el juego soltó el mouse (sdk/mouse_grab.h), ya muestra su propio
    // cursor; si no, ImGui dibuja uno.
    io.MouseDrawCursor = menuOpen && !sdk::mouse::ReleasedByLuma();
    if (menuOpen) {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    }

    // El vidrio solo usa el desenfoque si en este frame se va a desenfocar
    // (con el menú visible); si no, la textura tendría una imagen vieja.
    const bool glassVisible = menuOpen || menu::IsAnimatingClose();
    gpu::SetBlurActive(glassVisible && theme::BlurAmount() > 0.0f);

    // 1. ImGui: describir la interfaz de este frame. Orden de dibujo (de
    //    abajo hacia arriba): HUD de los módulos, menú, avisos.
    if (g_r.dx12Backend) {
        ImGui_ImplDX12_NewFrame();
    } else {
        ImGui_ImplDX11_NewFrame();
    }
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // En los menús del juego (el juego soltó el mouse) el HUD se esconde para
    // no quedar encima de ellos. Con el menú de Luma abierto sí se dibuja.
    const bool inGameMenu = !menuOpen && hud::GetStyle().hideInGameMenus && sdk::mouse::GameMouseState() == 0;
    music::DrawWorldEffects();  // lyrics and visualizer in the sky (also with the HUD hidden)
    hud::UpdateGameHudState(sdk::mouse::GameMouseState() != 0 && !menuOpen, sdk::camera::CurrentPerspective() <= 0, !menuOpen);
    const bool f1Hidden = hud::F1Hidden() && theme::Get().f1HidesHud && !menuOpen;
    const bool drawHud = !inGameMenu && !(cinematic::HideHud() && !menuOpen) && !f1Hidden;
    if (drawHud) {
        modules::Manager::RenderHud(menuOpen && hudeditor::IsActive());
    }
    cinematic::Draw();  // barras de cine / catalejo del zoom
    // También mientras se desvanece al cerrarse (ya sin recibir input).
    if (menuOpen || menu::IsAnimatingClose()) {
        menu::Draw(menu::RenderInfo{g_r.width, g_r.height, g_r.bufferCount, FormatName(g_r.format)});
    }
    notify::Render();
    loading::Render();
    icons::Flush();  // the icons drawn for the first time this frame, to the GPU
    ImGui::Render();
    g_wantsTextInput.store(menuOpen && io.WantTextInput, std::memory_order_relaxed);
    // Los números de textura de Luma -> las texturas reales de esta GPU.
    gpu::TranslateDrawData(ImGui::GetDrawData());

    if (g_r.dx11Backend) {
        // DirectX 11: sin vidrio esmerilado ni post-procesado (ver gpu.h).
        SubmitDx11(ImGui::GetDrawData());
        return;
    }

    const UINT index = swapchain3->GetCurrentBackBufferIndex();
    if (index >= g_r.bufferCount) {
        return;
    }
    FrameContext& frame = g_r.frames[index];

    // 2. Esperar a que la GPU haya terminado con este allocator (lo usamos la
    //    última vez que este mismo buffer se mostró). Casi nunca hay que esperar.
    WaitForFenceValue(frame.fenceValue);
    frame.allocator->Reset();
    g_r.commandList->Reset(frame.allocator.Get(), nullptr);

    // 3. "Barrera": avisar a la GPU que el buffer pasa de "listo para mostrar"
    //    a "se va a dibujar encima". Al final, lo devolvemos al estado original.
    //    Con el menú visible, antes copiamos el frame del juego y lo
    //    desenfocamos para el vidrio (render/gpu.h); RecordBlur deja el buffer
    //    listo para dibujar.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.backBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // Primero el post-procesado (cambia los colores del juego) y después, si
    // el menú está abierto, el desenfoque del vidrio (sobre el frame ya
    // procesado). El menú y el HUD de Luma se dibujan encima, sin procesar.
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;
    if (postfx::Active() && gpu::PostReady()) {
        gpu::RecordPost(g_r.commandList.Get(), frame.backBuffer.Get(), frame.rtv, postfx::Get());
        state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (glassVisible && theme::BlurAmount() > 0.0f) {
        gpu::RecordBlur(g_r.commandList.Get(), frame.backBuffer.Get(), theme::BlurAmount(), state);
    } else if (state == D3D12_RESOURCE_STATE_PRESENT) {
        g_r.commandList->ResourceBarrier(1, &barrier);
    }

    // 4. Dibujar ImGui.
    g_r.commandList->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {gpu::SrvHeap()};
    g_r.commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_r.commandList.Get());

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_r.commandList->ResourceBarrier(1, &barrier);
    g_r.commandList->Close();

    // 5. Mandar los comandos a la cola del juego y marcar el fence.
    ID3D12CommandQueue* queue = g_queue.load();
    ID3D12CommandList* lists[] = {g_r.commandList.Get()};
    queue->ExecuteCommandLists(1, lists);
    frame.fenceValue = ++g_r.fenceCounter;
    queue->Signal(g_r.fence.Get(), frame.fenceValue);
}

}  // namespace

// =============================================================================
//  Funciones públicas
// =============================================================================

}  // namespace luma::render::overlay

// Ver imgui_config.h. Va fuera del namespace porque ImGui la busca por su
// nombre "global".
#ifndef NDEBUG
void LumaImGuiAssert(const char* expression, const char* file, int line) {
    luma::log::Error("ImGui assert: {} ({}:{})", expression, file, line);
}
#endif

namespace luma::render::overlay {

bool OnPresent(IDXGISwapChain* swapchain) {
    State state = g_state.load(std::memory_order_acquire);

    if (state == State::Waiting) {
        // En DirectX 12 hace falta saber la cola del juego; en DirectX 11 no.
        if (g_queue.load(std::memory_order_acquire) == nullptr && ApiOf(swapchain) != Api::Dx11) {
            ID3D12CommandQueue* own = dx12::QueueFor(swapchain);
            if (own == nullptr) {
                return false;  // Todavía no sabemos qué cola usa el juego.
            }
            own->Release();  // (solo queríamos saber si la conocemos)
        }
        // Si Luma se cargó con el juego recién abierto, el juego crea y tira
        // varias swapchains al arrancar. Engancharnos a una intermedia la deja
        // "agarrada" y el juego no puede crear la siguiente (pantalla negra).
        // Esperamos a que deje de crear swapchains un rato.
        //
        // Y además: mientras arranca, el juego mide cómo anda DirectX 12 y, si
        // ve un tirón (nuestro primer frame tarda ~150 ms: fuentes, texturas),
        // se pasa a DirectX 11, donde el overlay no dibuja. Por eso esperamos
        // a que muestre su primera pantalla "de verdad" (la de inicio o el
        // HUD), o 45 segundos si no podemos saberlo.
        static const ULONGLONG startedAt = GetTickCount64();
        // (Solo si Luma se cargó temprano: si llegó con el juego ya abierto, no
        // vio crearse ninguna swapchain y no hay nada que esperar.)
        const bool uiReady = !dx12::SawGameSwapchainCreated() || sdk::paperdoll::GameUiReady() ||
                             GetTickCount64() - startedAt > 45000;
        if (!dx12::SwapchainSettled() || !uiReady) {
            return false;
        }
        std::lock_guard lock(StateMutex());
        if (g_state.load() == State::Waiting) {
            Initialize(swapchain);
        }
        // Este frame hizo el trabajo de inicialización (salió bien o no).
        return true;
    }
    if (state != State::Ready) {
        return false;
    }
    if (swapchain != g_r.swapchain && g_r.replaced) {
        // La swapchain nueva del juego (quizá de otro DirectX): la adoptamos.
        std::lock_guard lock(StateMutex());
        if (!AdoptSwapchain(swapchain) && !g_r.loggedOtherSwapchain) {
            g_r.loggedOtherSwapchain = true;
            log::Warn("Overlay: todavía no se pudo pasar a la swapchain nueva ({}); se reintenta.",
                      ApiName(ApiOf(swapchain)));
        }
        return false;
    }
    if (swapchain != g_r.swapchain) {
        if (!g_r.loggedOtherSwapchain) {
            g_r.loggedOtherSwapchain = true;
            log::Warn("Se detectó otra swapchain distinta de la inicial; se ignora.");
        }
        return false;
    }

    // CAMINO RÁPIDO: menú cerrado, ningún módulo dibujando y ningún aviso en
    // pantalla = no hay nada que dibujar. Son tres lecturas de variables
    // atómicas; ImGui ni se toca. Así, con 0 módulos activos, Luma cuesta lo
    // mismo que en la Fase 2.
    if (!g_menuOpen.load(std::memory_order_relaxed) && !modules::Manager::HasHud() &&
        !notify::HasActive() && !menu::IsAnimatingClose() && !loading::IsVisible() && !postfx::Active() && !cinematic::Active()) {
        return false;
    }
    // Benchmark en modo "Luma en reposo": no dibujar nada (ni HUD ni avisos)
    // mientras graba, para medir el juego casi como si Luma no estuviera.
    if (perf::benchmark::SuppressDrawing() && !g_menuOpen.load(std::memory_order_relaxed)) {
        return false;
    }

    std::lock_guard lock(StateMutex());
    RenderFrame(swapchain);
    return false;
}

void BeforeResize(IDXGISwapChain* swapchain) {
    if (g_state.load() != State::Ready || swapchain != g_r.swapchain) {
        return;
    }
    std::lock_guard lock(StateMutex());
    // ResizeBuffers falla si alguien todavía tiene referencias a los buffers
    // viejos, así que soltamos las nuestras.
    DestroyDeviceResources();
    g_r.resizing = true;
}

HWND GameWindowForOverlay() {
    return g_state.load() == State::Ready ? g_r.window : nullptr;
}

void BeforeSwapchainReplaced(HWND window) {
    if (g_state.load() != State::Ready || window != g_r.window) {
        return;  // (otra ventana: no es la nuestra)
    }
    std::lock_guard lock(StateMutex());
    // Soltar todo lo nuestro de la swapchain vieja (buffers incluidos): si no,
    // el juego no puede crear otra en la misma ventana ("acceso denegado").
    DestroyDeviceResources();
    g_r.replaced = true;
    // Olvidar cuál era: la swapchain nueva puede quedar en la MISMA dirección
    // de memoria que la vieja (Windows reusa el lugar liberado) y la
    // confundiríamos con ella, sin adoptarla nunca (Luma invisible).
    g_r.swapchain = nullptr;
}

void AfterResize(IDXGISwapChain* swapchain, HRESULT result) {
    std::lock_guard lock(StateMutex());
    if (!g_r.resizing || swapchain != g_r.swapchain) {
        return;
    }
    g_r.resizing = false;

    // Aunque el cambio de tamaño haya fallado, los buffers viejos siguen ahí y
    // podemos volver a usarlos: en ambos casos recreamos.
    if (!CreateDeviceResources(swapchain)) {
        log::Error("No se pudieron recrear los recursos tras cambiar el tamaño. Overlay desactivado.");
        g_state.store(State::Failed);
        return;
    }
    log::Info("Tamaño cambiado: {}x{} ({}).", g_r.width, g_r.height,
              SUCCEEDED(result) ? "ok" : "el juego reportó error");
}

void CaptureQueue(ID3D12CommandQueue* queue) {
    if (g_queue.load(std::memory_order_relaxed) != nullptr) {
        return;  // Ya la tenemos.
    }
    const State state = g_state.load(std::memory_order_relaxed);
    if (state != State::Waiting && state != State::Ready) {
        return;  // El overlay está desactivado. (Ready sin cola: el juego cambió de DirectX.)
    }
    // Solo nos sirve una cola "directa" (la que puede dibujar). El juego
    // también puede tener colas de cómputo o de copia.
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }
    // AddRef: le decimos a DX12 "yo también estoy usando esta cola", para que
    // no se destruya mientras la tengamos guardada.
    queue->AddRef();
    ID3D12CommandQueue* expected = nullptr;
    if (!g_queue.compare_exchange_strong(expected, queue)) {
        queue->Release();  // Otro hilo la capturó primero.
        return;
    }
    log::Info("Cola de comandos del juego capturada.");
}

void ToggleMenu() {
    if (g_state.load() != State::Ready) {
        return;  // Sin overlay no hay menú que mostrar.
    }
    const bool open = !g_menuOpen.load();
    {
        std::lock_guard lock(StateMutex());
        if (open) {
            menu::OnOpened();
        } else {
            if (menu::IsCapturingKey()) {
                menu::OnKeyCaptured(VK_ESCAPE);  // cerrar el menú cancela la asignación
            }
            menu::OnClosed();
        }
    }
    g_menuOpen.store(open);
    if (!open) {
        g_wantsTextInput.store(false);
    }
    input::OnMenuToggled(open);  // liberar / restaurar el cursor
}

bool OnKeyDown(int virtualKey) {
    std::lock_guard lock(StateMutex());
    if (g_menuOpen.load() && menu::IsCapturingKey()) {
        menu::OnKeyCaptured(virtualKey);
        return true;  // la tecla era para el menú: el juego no la ve
    }
    if (modules::Manager::HandleKey(virtualKey)) {
        config::MarkDirty();
    }
    return false;  // las teclas de los módulos también le llegan al juego
}

bool IsCapturingKey() {
    std::lock_guard lock(StateMutex());
    return g_menuOpen.load() && menu::IsCapturingKey();
}

bool IsMenuOpen() {
    return g_menuOpen.load(std::memory_order_relaxed);
}

bool HandleEscape() {
    std::lock_guard lock(StateMutex());
    if (hudeditor::IsActive()) {
        hudeditor::End();
        return true;
    }
    if (worldmap::IsActive() && worldmap::Escape()) return true;  // (closes the map's open window first)
    return false;
}

bool WantsTextInput() {
    return g_wantsTextInput.load(std::memory_order_relaxed);
}

void FeedInput(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_state.load() != State::Ready) {
        return;
    }
    std::lock_guard lock(StateMutex());
    if (g_r.win32Backend) {
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
    }
}

void Shutdown() {
    std::lock_guard lock(StateMutex());
    g_menuOpen.store(false);

    ReleaseDevice();  // (ImGui de DX11/12, fence, cola y GPU timer)
    icons::Shutdown();
    gpu::Shutdown();
    if (g_r.win32Backend) {
        ImGui_ImplWin32_Shutdown();
        g_r.win32Backend = false;
    }
    if (g_r.imguiContext) {
        ImGui::DestroyContext();
        g_r.imguiContext = false;
    }
    g_r.swapchain = nullptr;
    g_r.window = nullptr;
    g_state.store(State::Failed);  // Nada más se dibuja hasta la próxima carga.
}

}  // namespace luma::render::overlay
