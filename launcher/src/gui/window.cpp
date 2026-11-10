// =============================================================================
//  window.cpp - La ventana del launcher (Win32 + Direct3D 11 + Dear ImGui)
// =============================================================================
//  QUÉ HACE ESTE ARCHIVO
//    1. Crea una ventana SIN el borde estándar de Windows (dibujamos nuestra
//       propia barra de título), pero que igual se puede mover, agrandar desde
//       los bordes, maximizar (incluso con Win+Flechas) y que tiene la sombra
//       y las esquinas redondeadas de Windows 11.
//    2. Crea un "lienzo" de Direct3D 11 (la API de gráficos de Windows) donde
//       Dear ImGui dibuja la interfaz. D3D11 es mucho más simple que D3D12 (el
//       que usa el juego) y de sobra para una ventana como esta.
//    3. El bucle de mensajes: Windows nos avisa de todo (mouse, teclado,
//       cambio de tamaño...) con "mensajes"; los atendemos y dibujamos.
//
//  CÓMO SE QUITA EL BORDE: la ventana tiene estilo normal (con borde
//  redimensionable), pero al recibir WM_NCCALCSIZE le decimos a Windows "toda
//  la ventana es área nuestra" (cliente). Después, en WM_NCHITTEST, le
//  decimos qué es cada punto: un borde para agrandar, la barra para
//  arrastrar, o contenido normal.
//
//  RENDIMIENTO: con la ventana minimizada no se dibuja nada (el hilo duerme
//  hasta el próximo mensaje). Visible, se dibuja sincronizado con la pantalla.
// =============================================================================
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cwchar>

#include "gui/app.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using namespace luma::launcher::gui;

constexpr int kStartWidth = 1280;  // tamaño inicial (a 100% de escala)
constexpr int kStartHeight = 760;
constexpr int kMinWidth = 980;
constexpr int kMinHeight = 620;
constexpr int kResizeBorder = 6;   // píxeles (a 100%) desde el borde que sirven para agrandar

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain1* g_swapChain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
float g_scale = 1.0f;
bool g_minimized = false;
bool g_ready = false;  // ImGui y la interfaz ya están listos (antes no se dibuja)

void CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

void ReleaseTarget() {
    if (g_target != nullptr) {
        g_target->Release();
        g_target = nullptr;
    }
}

bool CreateDevice(HWND hwnd) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &g_device, nullptr, &g_context);
    if (FAILED(hr)) {
        // Sin GPU compatible: dibujar con el procesador (más lento, pero funciona).
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device,
                               nullptr, &g_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // el modo moderno (y sin parpadeos al agrandar)
    hr = factory->CreateSwapChainForHwnd(g_device, hwnd, &desc, nullptr, nullptr, &g_swapChain);
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxgiDevice->Release();
    if (FAILED(hr)) {
        return false;
    }
    CreateTarget();
    return true;
}

void DestroyDevice() {
    ReleaseTarget();
    if (g_swapChain != nullptr) g_swapChain->Release();
    if (g_context != nullptr) g_context->Release();
    if (g_device != nullptr) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

float ScaleOf(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

void Render(HWND hwnd);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return 1;
    }
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) {
                // "Toda la ventana es nuestra". Maximizada, Windows la agranda
                // un poco más que la pantalla (por el borde invisible): la
                // recortamos para que no se salga.
                if (IsZoomed(hwnd)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    const UINT dpi = GetDpiForWindow(hwnd);
                    const int frame = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    params->rgrc[0].left += frame;
                    params->rgrc[0].right -= frame;
                    params->rgrc[0].top += frame;
                    params->rgrc[0].bottom -= frame;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &p);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (!IsZoomed(hwnd)) {
                const int b = static_cast<int>(kResizeBorder * g_scale);
                const bool left = p.x < b, right = p.x >= rc.right - b, top = p.y < b, bottom = p.y >= rc.bottom - b;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            return AppIsCaption(p.x, p.y) ? HTCAPTION : HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * g_scale);
            info->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * g_scale);
            return 0;
        }
        case WM_SIZE:
            g_minimized = wParam == SIZE_MINIMIZED;
            if (g_swapChain != nullptr && !g_minimized) {
                ReleaseTarget();
                g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateTarget();
                Render(hwnd);  // dibujar ya, así al agrandar no se ve basura
            }
            return 0;
        case WM_DPICHANGED: {
            g_scale = static_cast<float>(HIWORD(wParam)) / 96.0f;
            AppSetScale(g_scale);
            const RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU) {
                return 0;  // que Alt no abra el menú invisible de la ventana
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Render(HWND hwnd) {
    (void)hwnd;
    // Nunca dibujar un frame adentro de otro (Windows puede mandarnos
    // WM_SIZE mientras dibujamos: ImGui no lo soporta y el programa se caía).
    static bool rendering = false;
    if (!g_ready || g_target == nullptr || rendering) {
        return;
    }
    rendering = true;
    struct Done {
        ~Done() { rendering = false; }
    } done;
    if (AppFontsDirty()) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        AppRebuildFonts();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    AppFrame();
    ImGui::Render();
    const float clear[4] = {0, 0, 0, 1};
    g_context->OMSetRenderTargets(1, &g_target, nullptr);
    g_context->ClearRenderTargetView(g_target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swapChain->Present(1, 0);  // 1 = esperar a la pantalla (vsync): no gasta de más
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));  // el ícono de luma.rc
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"LumaLauncherWindow";
    RegisterClassExW(&wc);

    // Tamaño inicial según la escala de la pantalla principal, centrado.
    const UINT systemDpi = GetDpiForSystem();
    g_scale = systemDpi > 0 ? static_cast<float>(systemDpi) / 96.0f : 1.0f;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(static_cast<int>(kStartWidth * g_scale), static_cast<int>(work.right - work.left));
    const int height = std::min(static_cast<int>(kStartHeight * g_scale), static_cast<int>(work.bottom - work.top));
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + (work.bottom - work.top - height) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Luma", WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                                nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        return 1;
    }
    g_scale = ScaleOf(hwnd);
    // Sombra y esquinas redondeadas de Windows 11 aunque no tengamos borde.
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    const DWORD corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    // Que Windows recalcule el marco con nuestro WM_NCCALCSIZE.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    if (!CreateDevice(hwnd)) {
        DestroyDevice();
        MessageBoxW(nullptr, L"Direct3D 11 could not be started on this PC.", L"Luma", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // sin imgui.ini junto al launcher
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    AppSetDevice(g_device);
    AppInit(hwnd, g_scale);
    g_ready = true;

    // "--minimized": lo usa "Abrir con Windows" (arranca minimizado, sin robar el foco).
    const bool startMinimized = std::wcsstr(GetCommandLineW(), L"--minimized") != nullptr;
    ShowWindow(hwnd, startMinimized ? SW_SHOWMINNOACTIVE : (showCommand == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL));
    UpdateWindow(hwnd);

    bool running = true;
    while (running) {
        MSG msg;
        // Minimizada: dormir hasta el próximo mensaje (0% de CPU y GPU).
        if (g_minimized) {
            WaitMessage();
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
        if (running && !g_minimized) {
            Render(hwnd);
        }
    }

    AppShutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDevice();
    return 0;
}

#ifndef NDEBUG
// imgui_config.h (compartido con Luma.dll) redirige los "assert" de ImGui a
// esta función en Debug: en el launcher basta con avisar en el depurador.
void LumaImGuiAssert(const char* expression, const char* file, int line) {
    char text[512];
    wsprintfA(text, "ImGui assert: %s (%s:%d)\n", expression, file, line);
    OutputDebugStringA(text);
}
#endif
