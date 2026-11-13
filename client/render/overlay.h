// =============================================================================
//  overlay.h - El overlay de Luma: ImGui dibujado encima del juego con DX12
// =============================================================================
//  Los detours de dx12_hooks.cpp llaman a estas funciones. Aquí vive todo el
//  estado de dibujo: el contexto de ImGui y los objetos de DirectX 12 que
//  necesitamos para dibujar sobre la imagen del juego.
// =============================================================================
#pragma once

#include <windows.h>

struct IDXGISwapChain;
struct ID3D12CommandQueue;

namespace luma::render::overlay {

// --- Llamadas desde los hooks de DirectX (hilo de render del juego) ------------

// Una vez por frame, justo antes de que el juego muestre la imagen.
// Devuelve true solo en el frame en que el overlay se inicializó (trabajo
// que ocurre una vez y no debe contarse como costo por frame).
bool OnPresent(IDXGISwapChain* swapchain);

// Antes y después de que el juego cambie el tamaño de la swapchain.
void BeforeResize(IDXGISwapChain* swapchain);
void AfterResize(IDXGISwapChain* swapchain, HRESULT result);
// El juego va a crear OTRA swapchain (render/dx12_hooks): soltar la vieja.
void BeforeSwapchainReplaced(HWND window);
// La ventana donde dibuja el overlay (nullptr si todavía no está listo).
HWND GameWindowForOverlay();

// Cada vez que el juego manda trabajo a la GPU (para descubrir su cola).
void CaptureQueue(ID3D12CommandQueue* queue);

// --- Llamadas desde el procesador de mensajes de la ventana (input.cpp) --------

void ToggleMenu();
bool IsMenuOpen();

// Esc con el menú abierto: si el editor de HUD está abierto, lo cierra y
// vuelve al menú (devuelve true). Si no, devuelve false (el menú se cierra).
bool HandleEscape();

// ¿ImGui está esperando que el usuario escriba texto?
bool WantsTextInput();

// Se pulsó una tecla (no repetición). Asigna la tecla si el menú la está
// esperando, o activa/desactiva los módulos que la tengan. Devuelve true si
// la tecla era para Luma y el juego no debe verla.
bool OnKeyDown(int virtualKey);

// ¿El menú está esperando una tecla para asignarla?
bool IsCapturingKey();

// Pasa un mensaje de teclado/mouse a ImGui de forma segura entre hilos.
void FeedInput(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// --- Llamada desde el apagado de Luma ----------------------------------------------

// Espera a que la GPU termine nuestro trabajo y libera todo (ImGui y DX12).
// Solo debe llamarse cuando ya ningún hook puede ejecutarse.
void Shutdown();

}  // namespace luma::render::overlay
