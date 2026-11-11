// =============================================================================
//  frame_stats.cpp - Measurement implementation
// =============================================================================
#include "perf/frame_stats.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "perf/benchmark.h"
#include "perf/timer.h"

namespace luma::perf {

namespace {

History g_history;
std::int64_t g_lastFrameStart = 0;

// Accumulators shared between the render thread (writes every frame) and
// Luma's main thread (reads every 30 s). They're atomic so both can use
// them at the same time without locks: adding to an atomic costs a few
// nanoseconds.
std::atomic<std::uint64_t> g_totalFrames{0};
std::atomic<std::uint64_t> g_closedFrames{0};
std::atomic<std::uint64_t> g_closedCostTicks{0};
std::atomic<std::uint64_t> g_closedMaxTicks{0};

// Only the main thread (TakeSummary) and the menu (LastSummary) use them.
std::int64_t g_periodStart = 0;
std::mutex g_summaryMutex;
Summary g_lastSummary;

std::atomic<std::uint32_t> g_renderThread{0};

}  // namespace

void RecordFrame(std::int64_t frameStart, std::int64_t lumaEnd, bool menuOpen, bool oneTimeWork) {
    const std::int64_t cost = oneTimeWork ? 0 : lumaEnd - frameStart;
    if (g_renderThread.load(std::memory_order_relaxed) == 0) {
        g_renderThread.store(GetCurrentThreadId());
    }

    // History (the first frame has no "previous" one, so there's no frametime).
    if (g_lastFrameStart != 0) {
        const std::size_t i = g_history.next;
        g_history.frameMs[i] = static_cast<float>(TicksToMs(frameStart - g_lastFrameStart));
        g_history.costUs[i] = static_cast<float>(TicksToUs(cost));
        g_history.menuOpen[i] = menuOpen;
        // The benchmark (if it's recording) stores every frame.
        benchmark::OnFrame(frameStart, g_history.frameMs[i], g_history.costUs[i]);
        g_history.next = (i + 1) % kHistorySize;
        if (g_history.count < kHistorySize) {
            ++g_history.count;
        }
    }
    g_lastFrameStart = frameStart;

    // Accumulators. "memory_order_relaxed" = we only care that the final
    // number is right, not the exact order relative to other variables.
    g_totalFrames.fetch_add(1, std::memory_order_relaxed);
    if (!menuOpen && !oneTimeWork) {
        const auto costTicks = static_cast<std::uint64_t>(cost);
        g_closedFrames.fetch_add(1, std::memory_order_relaxed);
        g_closedCostTicks.fetch_add(costTicks, std::memory_order_relaxed);
        // Only this thread writes the max, so "read and compare" is enough.
        if (costTicks > g_closedMaxTicks.load(std::memory_order_relaxed)) {
            g_closedMaxTicks.store(costTicks, std::memory_order_relaxed);
        }
    }
}

const History& GetHistory() {
    return g_history;
}

Recent RecentAverage(std::size_t frames) {
    Recent r;
    const std::size_t n = std::min(frames, g_history.count);
    if (n == 0) {
        return r;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t index = (g_history.next + kHistorySize - 1 - i) % kHistorySize;
        r.frameMs += g_history.frameMs[index];
        r.costUs += g_history.costUs[index];
    }
    r.frameMs /= static_cast<double>(n);
    r.costUs /= static_cast<double>(n);
    return r;
}

std::uint32_t RenderThreadId() {
    return g_renderThread.load(std::memory_order_relaxed);
}

Summary TakeSummary() {
    const std::int64_t now = Now();
    // "exchange(0)" reads the value and sets it to 0 in a single step.
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
