// =============================================================================
//  clicks.cpp - Implementación del registro de clics
// =============================================================================
#include "core/clicks.h"

#include <array>
#include <atomic>
#include <cstdint>

#include "perf/timer.h"

namespace luma::clicks {

namespace {

// 64 clics por botón es de sobra: nadie hace 64 clics en un segundo.
constexpr std::size_t kSlots = 64;

struct Ring {
    std::array<std::atomic<std::int64_t>, kSlots> times{};  // momento de cada clic (ticks)
    std::atomic<std::uint32_t> next{0};
};

std::array<Ring, 2> g_rings;
std::atomic<int> g_users{0};

}  // namespace

void Acquire() {
    g_users.fetch_add(1);
}

void Release() {
    if (g_users.fetch_sub(1) <= 0) {
        g_users.store(0);
    }
}

bool Wanted() {
    return g_users.load(std::memory_order_relaxed) > 0;
}

void Record(Button button) {
    Ring& ring = g_rings[static_cast<std::size_t>(button)];
    const std::uint32_t slot = ring.next.fetch_add(1, std::memory_order_relaxed) % kSlots;
    ring.times[slot].store(perf::Now(), std::memory_order_relaxed);
}

int PerSecond(Button button) {
    const Ring& ring = g_rings[static_cast<std::size_t>(button)];
    const std::int64_t now = perf::Now();
    const std::int64_t window = perf::g_ticksPerSecond;  // 1 segundo en ticks
    int count = 0;
    for (const auto& t : ring.times) {
        const std::int64_t value = t.load(std::memory_order_relaxed);
        if (value != 0 && now - value <= window) {
            ++count;
        }
    }
    return count;
}

}  // namespace luma::clicks
