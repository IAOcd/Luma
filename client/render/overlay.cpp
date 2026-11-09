// =============================================================================
//  overlay.cpp - Dibujar ImGui encima del juego con DirectX 12
// =============================================================================
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

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "core/config.h"
#include "core/logger.h"
#include "core/paths.h"
#include "core/state.h"
#include "modules/module_manager.h"
#include "perf/benchmark.h"
#include "render/assets.h"
#include "render/gpu.h"
#include "render/hud.h"
#include "render/hud_editor.h"
#include "render/input.h"
#include "render/item_icons.h"
#include "render/loading.h"
#include "render/menu.h"
#include "render/notifications.h"
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
    Unsupported,  // El juego no usa DX12: overlay desactivado.
    Failed,       // Algo falló al crear recursos: overlay desactivado.
};

// Todo el estado de dibujo en un solo lugar.
struct Renderer {
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

    bool imguiContext = false;
    bool win32Backend = false;
    bool dx12Backend = false;
    bool resizing = false;
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
    for (FrameContext& frame : g_r.frames) {
        frame = FrameContext{};  // libera allocator y back buffer (ComPtr)
    }
    gpu::DestroyBlurTargets();  // dependen del tamaño de la pantalla
    g_r.commandList.Reset();
    g_r.rtvHeap.Reset();
    g_r.bufferCount = 0;
}

bool CreateDeviceResources(IDXGISwapChain3* swapchain) {
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

// --- ImGui -----------------------------------------------------------------------

// Carga las fuentes Segoe UI de Windows (tienen tildes y ñ) al tamaño del
// tema: normal para el texto, más grande para los nombres de módulos y
// seminegrita para los títulos. Si no están, usa la de ImGui.
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    theme::Fonts& fonts = theme::GetFonts();
    fonts = theme::Fonts{};

    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    const std::filesystem::path fontsDir = std::filesystem::path(windowsDir) / L"Fonts";
    auto load = [&](const wchar_t* file, float size) -> ImFont* {
        std::error_code ec;
        const std::filesystem::path path = fontsDir / file;
        if (!std::filesystem::exists(path, ec)) {
            return nullptr;
        }
        return io.Fonts->AddFontFromFileTTF(paths::ToUtf8(path).c_str(), size);
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
    // Las del HUD (la pixelada del juego a varios tamaños exactos y la suave).
    hud::LoadFonts(io, theme::UiScale());
}

// El tamaño de letra cambió: regenerar la textura de la fuente en la GPU.
// Pasa solo cuando el usuario suelta el slider de "Tamaño de letra".
void RebuildFonts() {
    WaitForGpu();  // la textura vieja podría estar en uso
    LoadFonts();
    ImGui_ImplDX12_InvalidateDeviceObjects();
    ImGui_ImplDX12_CreateDeviceObjects();
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

// Primera vez que vemos un frame con la cola ya capturada: preparar todo.
// Se llama con g_mutex tomado.
bool Initialize(IDXGISwapChain* swapchain) {
    ComPtr<IDXGISwapChain3> swapchain3;
    ComPtr<ID3D12Device> device;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
        FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        log::Warn("El juego no está usando DirectX 12 (¿quizá DirectX 11?). "
                  "El overlay queda desactivado; el resto de Luma sigue funcionando.");
        g_state.store(State::Unsupported);
        return false;
    }

    // Comprobar que la cola capturada pertenece al mismo dispositivo que la
    // swapchain. Si no, la descartamos y esperamos a capturar otra.
    ID3D12CommandQueue* queue = g_queue.load();
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice != device) {
        log::Warn("La cola capturada no corresponde a la swapchain; se buscará otra.");
        if (g_queue.compare_exchange_strong(queue, nullptr)) {
            queue->Release();
        }
        return false;
    }

    g_r.swapchain = swapchain;
    g_r.device = device;
    swapchain3->GetHwnd(&g_r.window);

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_r.fence)))) {
        log::Error("No se pudo crear el fence de DX12.");
        g_state.store(State::Failed);
        return false;
    }
    g_r.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Tabla de descriptores, subida de texturas y desenfoque (render/gpu.h).
    if (!gpu::Init(device.Get(), queue)) {
        g_state.store(State::Failed);
        return false;
    }

    // Necesitamos el alto de la imagen antes de crear ImGui (para la escala).
    DXGI_SWAP_CHAIN_DESC1 desc{};
    swapchain3->GetDesc1(&desc);
    g_r.height = desc.Height;

    CreateImGui();
    if (!CreateDeviceResources(swapchain3.Get())) {
        g_state.store(State::Failed);
        return false;
    }

    input::Install(g_r.window);

    log::Info("Overlay listo: DirectX 12, {}x{}, formato {}, {} buffers.", g_r.width, g_r.height,
              FormatName(g_r.format), g_r.bufferCount);
    log::Info("Pulsa L para abrir el menú.");
    g_state.store(State::Ready);
    return true;
}

// Dibuja un frame de ImGui sobre el back buffer actual. Con g_mutex tomado.
void RenderFrame(IDXGISwapChain* swapchain) {
    if (!g_r.dx12Backend) {
        return;  // En medio de un cambio de tamaño.
    }
    ComPtr<IDXGISwapChain3> swapchain3;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3)))) {
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
    gpu::SetBlurActive(glassVisible && theme::Get().blur > 0.0f);

    // 1. ImGui: describir la interfaz de este frame. Orden de dibujo (de
    //    abajo hacia arriba): HUD de los módulos, menú, avisos.
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // En los menús del juego (el juego soltó el mouse) el HUD se esconde para
    // no quedar encima de ellos. Con el menú de Luma abierto sí se dibuja.
    const bool inGameMenu = !menuOpen && hud::GetStyle().hideInGameMenus && sdk::mouse::GameMouseState() == 0;
    if (!inGameMenu) {
        modules::Manager::RenderHud(menuOpen && hudeditor::IsActive());
    }
    // También mientras se desvanece al cerrarse (ya sin recibir input).
    if (menuOpen || menu::IsAnimatingClose()) {
        menu::Draw(menu::RenderInfo{g_r.width, g_r.height, g_r.bufferCount, FormatName(g_r.format)});
    }
    notify::Render();
    loading::Render();
    ImGui::Render();
    g_wantsTextInput.store(menuOpen && io.WantTextInput, std::memory_order_relaxed);

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
    if (glassVisible && theme::Get().blur > 0.0f) {
        gpu::RecordBlur(g_r.commandList.Get(), frame.backBuffer.Get(), theme::Get().blur);
    } else {
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
        if (g_queue.load(std::memory_order_acquire) == nullptr) {
            return false;  // Todavía no sabemos qué cola usa el juego.
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
        !notify::HasActive() && !menu::IsAnimatingClose() && !loading::IsVisible()) {
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

void AfterResize(IDXGISwapChain* swapchain, HRESULT result) {
    std::lock_guard lock(StateMutex());
    if (!g_r.resizing || swapchain != g_r.swapchain) {
        return;
    }
    g_r.resizing = false;

    ComPtr<IDXGISwapChain3> swapchain3;
    // Aunque el cambio de tamaño haya fallado, los buffers viejos siguen ahí y
    // podemos volver a usarlos: en ambos casos recreamos.
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
        !CreateDeviceResources(swapchain3.Get())) {
        log::Error("No se pudieron recrear los recursos tras cambiar el tamaño. Overlay desactivado.");
        g_state.store(State::Failed);
        return;
    }
    log::Info("Tamaño cambiado: {}x{} ({}).", g_r.width, g_r.height,
              SUCCEEDED(result) ? "ok" : "el juego reportó error");
}

void CaptureQueue(ID3D12CommandQueue* queue) {
    if (g_queue.load(std::memory_order_relaxed) != nullptr ||
        g_state.load(std::memory_order_relaxed) != State::Waiting) {
        return;  // Ya la tenemos (o el overlay está desactivado).
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

    DestroyDeviceResources();
    gpu::Shutdown();
    if (g_r.win32Backend) {
        ImGui_ImplWin32_Shutdown();
        g_r.win32Backend = false;
    }
    if (g_r.imguiContext) {
        ImGui::DestroyContext();
        g_r.imguiContext = false;
    }
    g_r.fence.Reset();
    if (g_r.fenceEvent != nullptr) {
        CloseHandle(g_r.fenceEvent);
        g_r.fenceEvent = nullptr;
    }
    g_r.device.Reset();
    g_r.swapchain = nullptr;
    g_r.window = nullptr;

    if (ID3D12CommandQueue* queue = g_queue.exchange(nullptr)) {
        queue->Release();
    }
    g_state.store(State::Failed);  // Nada más se dibuja hasta la próxima carga.
}

}  // namespace luma::render::overlay
