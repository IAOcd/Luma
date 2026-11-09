// =============================================================================
//  sound.h - Efectos de sonido de la interfaz
// =============================================================================
//  Los sonidos NO vienen de archivos: Luma los "sintetiza" al arrancar,
//  calculando la onda de cada uno (tonos suaves con subida y caída de
//  volumen). Así no hay archivos ajenos ni nada que descargar.
//
//  Se reproducen con PlaySound (de Windows) en un hilo propio de Luma, para
//  que el hilo de render del juego nunca espere al sistema de audio.
// =============================================================================
#pragma once

namespace luma::sound {

enum class Effect {
    Open,       // abrir el menú
    Close,      // cerrar el menú
    Click,      // botón o tarjeta
    ToggleOn,   // interruptor encendido
    ToggleOff,  // interruptor apagado
    Hover,      // pasar el mouse sobre algo (muy suave)
};

// Genera los sonidos y arranca el hilo de audio.
void Init();

// Pide reproducir un efecto (no espera; se puede llamar desde cualquier hilo).
// Si los sonidos están desactivados en el tema, no hace nada.
void Play(Effect effect);

// El volumen cambió (0..1): regenerar los sonidos con el nuevo volumen.
void SetVolume(float volume);

// Detiene el hilo de audio (al descargar Luma).
void Shutdown();

}  // namespace luma::sound
