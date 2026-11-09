// =============================================================================
//  frame_stats.h - Medición de frametime y del costo de Luma
// =============================================================================
//  Dos números por cada frame:
//
//    - Frametime: cuánto tiempo pasó desde el frame anterior (en ms).
//      60 FPS = 16,7 ms; 144 FPS = 6,9 ms. Cuanto más bajo y más parejo, mejor.
//
//    - Costo de Luma: cuánto tiempo pasó el hilo de render del juego dentro
//      de NUESTRO código en ese frame (en µs, microsegundos; 1 ms = 1000 µs).
//      Es el tiempo que Luma le "roba" al juego.
//
//  Los guardamos de dos formas:
//
//    1. Historial de los últimos 512 frames, en arreglos de tamaño fijo
//       reservados de antemano ("ring buffer": al llenarse, el dato nuevo pisa
//       al más viejo). Así NUNCA se pide memoria en el hot path. El menú lo
//       usa para el gráfico.
//
//    2. Acumuladores de "menú cerrado": suma y máximo del costo solo en los
//       frames con el menú cerrado. El hilo principal de Luma los lee y
//       reinicia cada 30 s y escribe un resumen en el log. Es la forma de
//       comprobar el criterio de la Fase 2 (< 0,1 ms) sin abrir el menú, que
//       cambiaría la medición.
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace luma::perf {

constexpr std::size_t kHistorySize = 512;

struct History {
    std::array<float, kHistorySize> frameMs{};  // frametime de cada frame
    std::array<float, kHistorySize> costUs{};   // costo de Luma en ese frame
    std::array<bool, kHistorySize> menuOpen{};  // ¿estaba el menú abierto?
    std::size_t next = 0;   // dónde se escribirá el próximo dato
    std::size_t count = 0;  // cuántos datos válidos hay (hasta kHistorySize)
};

// Registra un frame. "frameStart" es el momento en que el juego llamó a
// Present y "lumaEnd" el momento en que nuestro código terminó.
// "oneTimeWork" = en este frame Luma hizo un trabajo que ocurre una sola vez
// (inicializar el overlay). Se cuenta su frametime pero NO su costo, para no
// mezclar un costo único con el costo de cada frame (se informa aparte).
// Solo se llama desde el hilo de render del juego.
void RecordFrame(std::int64_t frameStart, std::int64_t lumaEnd, bool menuOpen, bool oneTimeWork);

// Historial para el menú. Solo leer desde el hilo de render (el mismo que
// escribe), así nunca se lee a medias.
const History& GetHistory();

// Resumen de un período con el menú cerrado.
struct Summary {
    bool valid = false;
    double seconds = 0.0;      // duración del período
    double averageFps = 0.0;   // FPS promedio (todos los frames)
    std::uint64_t closedFrames = 0;
    double averageCostUs = 0.0;  // costo promedio con el menú cerrado
    double maxCostUs = 0.0;      // costo máximo con el menú cerrado
};

// Toma el resumen desde la última llamada y reinicia los acumuladores.
// Se llama desde el hilo principal de Luma.
Summary TakeSummary();

// El último resumen tomado (para mostrarlo en el menú).
Summary LastSummary();

// Promedios de los últimos "frames" frames del historial (para el HUD).
// Solo desde el hilo de render.
struct Recent {
    double frameMs = 0.0;
    double costUs = 0.0;
};
Recent RecentAverage(std::size_t frames);

// El hilo de render del juego (el que llama a Present). 0 si todavía no se vio.
std::uint32_t RenderThreadId();

}  // namespace luma::perf
