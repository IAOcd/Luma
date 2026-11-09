// =============================================================================
//  gpu.h - Recursos de GPU de Luma: texturas y desenfoque (DirectX 12)
// =============================================================================
//  Tres cosas:
//
//  1. Una tabla de "descriptores" (SRV heap) que dura toda la sesión, con
//     espacio para 64 texturas. ImGui usa el primer lugar para su fuente; el
//     resto es para el desenfoque, el logo y lo que venga.
//
//  2. Subir texturas (imágenes RGBA) a la GPU para dibujarlas con ImGui.
//
//  3. El DESENFOQUE del efecto liquid glass. Con el menú abierto, en cada
//     frame:
//        a. copiamos la imagen del juego (el back buffer) a una textura;
//        b. la achicamos a 1/4 y la desenfocamos con un shader propio, en
//           varias pasadas horizontales y verticales (un "blur gaussiano");
//        c. ImGui dibuja esa textura desenfocada dentro de los paneles.
//     Un shader es un pequeño programa que corre en la GPU, uno por píxel.
//     Lo escribimos en HLSL (el lenguaje de shaders de DirectX) y se compila
//     al iniciar con D3DCompile.
//
//  Solo trabaja con el menú abierto: con el menú cerrado no cuesta nada.
//
//  HILOS: todo desde el hilo de render, con luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <d3d12.h>
#include <imgui.h>

#include <cstdint>

namespace luma::render::gpu {

// Lugar de la tabla reservado para la fuente de ImGui.
constexpr UINT kFontSlot = 0;

// Crea la tabla de descriptores y lo necesario para subir texturas y
// desenfocar. "queue" es la cola de comandos del juego.
bool Init(ID3D12Device* device, ID3D12CommandQueue* queue);

// Libera todo. Llamar con la GPU ya sin trabajo pendiente nuestro.
void Shutdown();

ID3D12DescriptorHeap* SrvHeap();
D3D12_CPU_DESCRIPTOR_HANDLE CpuSlot(UINT slot);
D3D12_GPU_DESCRIPTOR_HANDLE GpuSlot(UINT slot);

// Sube una imagen RGBA (4 bytes por píxel) a la GPU y devuelve el
// identificador para ImGui::Image / AddImage. 0 si falló. Espera a que termine.
ImTextureID CreateTexture(const std::uint8_t* rgba, UINT width, UINT height);

// --- Desenfoque -------------------------------------------------------------------

// (Re)crear las texturas del desenfoque para un tamaño de pantalla. Se llama
// al iniciar y al cambiar el tamaño de la ventana.
bool CreateBlurTargets(UINT width, UINT height, DXGI_FORMAT backBufferFormat);
void DestroyBlurTargets();

// Graba en la command list: copiar el back buffer y desenfocarlo. El back
// buffer tiene que estar en estado PRESENT; queda en RENDER_TARGET, listo
// para que ImGui dibuje encima. "strength" va de 0 a 1.
void RecordBlur(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, float strength);

// ¿Se va a desenfocar en este frame? Lo fija el overlay al empezar cada
// frame. Si no, BlurTexture() devuelve 0: la textura tendría una imagen vieja
// (de la última vez que el menú estuvo abierto).
void SetBlurActive(bool active);

// La textura desenfocada (para dibujarla en los paneles), o 0 si no hay.
ImTextureID BlurTexture();

}  // namespace luma::render::gpu
