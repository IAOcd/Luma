// =============================================================================
//  frame_stats.cpp - Implementación de la medición
// =============================================================================
#include "perf/frame_stats.h"

#include <atomic>
#include <mutex>

#include "perf/timer.h"

namespace luma::perf {

namespace {

History g_history;
std::int64_t g_lastFrameStart = 0;

// Acumuladores compartidos entre el hilo de render (escribe en cada frame) y
// el hilo principal de Luma (lee cada 30 s). Son atómicos para que ambos
// puedan usarlos a la vez sin candados: sumar a un atómico cuesta unos pocos
// nanosegundos.
std::atomic<std::uint64_t> g_totalFrames{0};
std::atomic<std::uint64_t> g_closedFrames{0};
std::atomic<std::uint64_t> g_closedCostTicks{0};
std::atomic<std::uint64_t> g_closedMaxTicks{0};

// Solo los usa el hilo principal (TakeSummary) y el menú (LastSummary).
std::int64_t g_periodStart = 0;
std::mutex g_summaryMutex;
Summary g_lastSummary;

}  // namespace

void RecordFrame(std::int64_t frameStart, std::int64_t lumaEnd, bool menuOpen, bool oneTimeWork) {
    const std::int64_t cost = oneTimeWork ? 0 : lumaEnd - frameStart;

    // Historial (el primer frame no tiene "anterior", así que no hay frametime).
    if (g_lastFrameStart != 0) {
        const std::size_t i = g_history.next;
        g_history.frameMs[i] = static_cast<float>(TicksToMs(frameStart - g_lastFrameStart));
        g_history.costUs[i] = static_cast<float>(TicksToUs(cost));
        g_history.menuOpen[i] = menuOpen;
        g_history.next = (i + 1) % kHistorySize;
        if (g_history.count < kHistorySize) {
            ++g_history.count;
        }
    }
    g_lastFrameStart = frameStart;

    // Acumuladores. "memory_order_relaxed" = solo nos importa que el número
    // final sea correcto, no el orden exacto respecto de otras variables.
    g_totalFrames.fetch_add(1, std::memory_order_relaxed);
    if (!menuOpen && !oneTimeWork) {
        const auto costTicks = static_cast<std::uint64_t>(cost);
        g_closedFrames.fetch_add(1, std::memory_order_relaxed);
        g_closedCostTicks.fetch_add(costTicks, std::memory_order_relaxed);
        // Solo este hilo escribe el máximo, así que "leer y comparar" alcanza.
        if (costTicks > g_closedMaxTicks.load(std::memory_order_relaxed)) {
            g_closedMaxTicks.store(costTicks, std::memory_order_relaxed);
        }
    }
}

const History& GetHistory() {
    return g_history;
}

Summary TakeSummary() {
    const std::int64_t now = Now();
    // "exchange(0)" lee el valor y lo deja en 0 en un solo paso.
    const std::uint64_t total = g_totalFrames.exchange(0, std::memory_order_relaxed);
    const std::uint64_t closed = g_closedFrames.exchange(0, std::memory_order_relaxed);
    const std::uint64_t costTicks = g_closedCostTicks.exchange(0, std::memory_order_relaxed);
    const std::uint64_t maxTicks = g_closedMaxTicks.exchange(0, std::memory_order_relaxed);

    Summary summary;
    if (g_periodStart != 0) {
        summary.seconds = TicksToMs(now - g_periodStart) / 1000.0;
        summary.valid = summary.seconds > 0.0 && total > 0;
        if (summary.valid) {
            summary.averageFps = static_cast<double>(total) / summary.seconds;
        }
        summary.closedFrames = closed;
        if (closed > 0) {
            summary.averageCostUs =
                TicksToUs(static_cast<std::int64_t>(costTicks)) / static_cast<double>(closed);
            summary.maxCostUs = TicksToUs(static_cast<std::int64_t>(maxTicks));
        }
    }
    g_periodStart = now;

    if (summary.valid) {
        std::lock_guard lock(g_summaryMutex);
        g_lastSummary = summary;
    }
    return summary;
}

Summary LastSummary() {
    std::lock_guard lock(g_summaryMutex);
    return g_lastSummary;
}

}  // namespace luma::perf
