// =============================================================================
//  dx12_hooks.h - Hooks de DXGI / DirectX 12
// =============================================================================
//  Interceptamos cuatro funciones de DirectX (de Windows, NO del juego):
//
//    IDXGISwapChain::Present          -> se llama una vez por frame para
//    IDXGISwapChain1::Present1           mostrar la imagen. Ahí dibujamos Luma.
//                                        (El juego usa una de las dos; hookeamos
//                                        ambas por las dudas.)
//    IDXGISwapChain::ResizeBuffers    -> cuando cambia el tamaño de la ventana.
//    IDXGISwapChain3::ResizeBuffers1     Hay que soltar nuestros recursos antes.
//    ID3D12CommandQueue::ExecuteCommandLists
//                                     -> por aquí el juego manda trabajo a la
//                                        GPU. Lo usamos solo para averiguar qué
//                                        "cola" usa el juego, porque en DX12 la
//                                        necesitamos para mandar nuestro dibujo.
//
//  Como son funciones de Windows, sus direcciones no cambian con las
//  actualizaciones de Minecraft: no hace falta escanear firmas para esto.
// =============================================================================
#pragma once

namespace luma::render::dx12 {

// Encuentra las funciones y crea los hooks (desactivados; se activan con
// hooks::EnableAll). Devuelve false si algo falla: en ese caso Luma sigue sin
// overlay, pero el juego no se ve afectado.
bool CreateHooks();

// Marca que Luma se está descargando: a partir de aquí los detours solo
// llaman a la función original, sin tocar nada nuestro.
void BeginUnload();

}  // namespace luma::render::dx12
