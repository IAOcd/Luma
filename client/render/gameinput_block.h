// =============================================================================
//  gameinput_block.h - Ocultarle mouse y teclado al juego con el menú abierto
// =============================================================================
//  Lo que descubrimos al probar la Fase 2: Minecraft GDK lee el mouse (y
//  probablemente el teclado) con GameInput, la API de input de Microsoft para
//  juegos. GameInput lee los dispositivos por su cuenta, sin pasar por los
//  mensajes de la ventana ni por raw input, así que los otros bloqueos no le
//  afectan.
//
//  Solución: hookear tres métodos de las "lecturas" de GameInput
//  (IGameInputReading):
//    - GetMouseState: con el menú abierto, el mouse aparece quieto y sin clics.
//    - GetKeyCount / GetKeyState: con el menú abierto, ninguna tecla aparece
//      apretada.
//
//  Son funciones de GameInputRedist.dll (de Windows, no del juego), así que no
//  dependen de la versión de Minecraft. El orden de los métodos viene del
//  header oficial GameInput.h del Windows SDK (versión 0 de la interfaz, la
//  que usa el juego: verificamos que su IID aparece en el ejecutable).
// =============================================================================
#pragma once

namespace luma::render::gameinput {

// Intenta instalar los hooks. Devuelve true cuando ya terminó (instalados, o
// no hacen falta porque el juego no usa GameInput) y false si hay que volver
// a intentarlo más tarde (por ejemplo, si todavía no hay ninguna lectura de
// mouse disponible). Se llama desde el hilo principal de Luma.
bool TryInstall();

// Desactivar el filtro antes de descargar Luma.
void BeginUnload();

}  // namespace luma::render::gameinput
