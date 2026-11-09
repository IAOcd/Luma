// =============================================================================
//  input_diag.h - Diagnóstico: ¿por dónde le llega el mouse al juego?
// =============================================================================
//  Con el menú abierto, el juego sigue moviendo la cámara aunque bloqueamos
//  los mensajes de la ventana, raw input y GameInput. Para dejar de adivinar,
//  contamos cuántas veces el juego usa cada vía y lo escribimos en el log.
//  La vía que tenga números altos mientras mueves el mouse es la culpable.
//
//  Contar cuesta un incremento atómico por llamada (nanosegundos). Es
//  temporal: se quita cuando el problema esté resuelto.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace luma::render::diag {

struct Counters {
    std::atomic<std::uint32_t> wndMouseMessages{0};  // WM_MOUSEMOVE, clics, rueda
    std::atomic<std::uint32_t> wndRawInputMessages{0};  // WM_INPUT
    std::atomic<std::uint32_t> getRawInputData{0};
    std::atomic<std::uint32_t> getRawInputBuffer{0};
    std::atomic<std::uint32_t> getCursorPosGame{0};  // llamadas que NO vienen de Luma
    std::atomic<std::uint32_t> gameInputGetReading{0};  // el juego pide lecturas a GameInput
    std::atomic<std::uint32_t> gameInputMouseState{0};
    std::atomic<std::uint32_t> gameInputKeyState{0};
    std::atomic<std::uint32_t> peekMessageBlocked{0};    // mensajes de input interceptados en la cola
    std::atomic<std::uint32_t> getAsyncKeyStateGame{0};  // llamadas del juego
    std::atomic<std::uint32_t> getKeyStateGame{0};       // llamadas del juego
};

inline Counters g_counters;

inline void Count(std::atomic<std::uint32_t>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}

// Escribe los contadores en el log y los pone en 0.
void LogAndReset(const char* context);

// Escribe en el log la "pila de llamadas" actual: la cadena de funciones que
// llevó hasta aquí, como "módulo+desplazamiento". Así sabemos QUÉ función del
// juego llamó a una función de Windows. Cada pila distinta se escribe una
// sola vez (como mucho 40), para no llenar el log.
void LogCallStack(const char* api, const char* detail);

}  // namespace luma::render::diag
