// =============================================================================
//  timer.h - Reloj de alta precisión
// =============================================================================
//  QueryPerformanceCounter (QPC) es el reloj más preciso de Windows: mide en
//  "ticks", normalmente de 100 nanosegundos (0,0001 ms). Leerlo cuesta unos
//  20-30 nanosegundos, así que se puede usar en cada frame sin problema.
//
//  Uso:
//      const std::int64_t start = perf::Now();
//      ...algo...
//      const double ms = perf::TicksToMs(perf::Now() - start);
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>

namespace luma::perf {

// Cuántos ticks hay en un segundo. Windows lo fija al arrancar la PC y no
// cambia, así que lo leemos una sola vez (en Init).
inline std::int64_t g_ticksPerSecond = 1;

inline void Init() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_ticksPerSecond = frequency.QuadPart;
}

// "inline" en funciones tan cortas le permite al compilador copiarlas en el
// lugar donde se usan, sin el costo de una llamada.
inline std::int64_t Now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

inline double TicksToMs(std::int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_ticksPerSecond);
}

inline double TicksToUs(std::int64_t ticks) {
    return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(g_ticksPerSecond);
}

}  // namespace luma::perf
