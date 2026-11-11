// =============================================================================
//  frame_stats.h - Measuring frametime and Luma's cost
// =============================================================================
//  Two numbers for every frame:
//
//    - Frametime: how much time passed since the previous frame (in ms).
//      60 FPS = 16.7 ms; 144 FPS = 6.9 ms. The lower and steadier, the better.
//
//    - Luma cost: how much time the game's render thread spent inside OUR
//      code in that frame (in µs, microseconds; 1 ms = 1000 µs).
//      It's the time Luma "steals" from the game.
//
//  We store them in two ways:
//
//    1. History of the last 512 frames, in fixed-size arrays reserved in
//       advance ("ring buffer": once full, the new value overwrites the
//       oldest one). That way memory is NEVER allocated in the hot path. The
//       menu uses it for the graph.
//
//    2. "Menu closed" accumulators: sum and max of the cost only for frames
//       with the menu closed. Luma's main thread reads and resets them every
//       30 s and writes a summary to the log. It's how the Phase 2 criterion
//       (< 0.1 ms) is checked without opening the menu, which would change
//       the measurement.
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace luma::perf {

constexpr std::size_t kHistorySize = 512;

struct History {
    std::array<float, kHistorySize> frameMs{};  // frametime of each frame
    std::array<float, kHistorySize> costUs{};   // Luma's cost in that frame
    std::array<bool, kHistorySize> menuOpen{};  // was the menu open?
    std::size_t next = 0;   // where the next value will be written
    std::size_t count = 0;  // how many valid values there are (up to kHistorySize)
};

// Records a frame. "frameStart" is when the game called Present and
// "lumaEnd" when our code finished.
// "oneTimeWork" = in this frame Luma did work that happens only once
// (initializing the overlay). Its frametime is counted but NOT its cost, so a
// one-time cost isn't mixed with the per-frame cost (it's reported separately).
// Only called from the game's render thread.
void RecordFrame(std::int64_t frameStart, std::int64_t lumaEnd, bool menuOpen, bool oneTimeWork);

// History for the menu. Only read from the render thread (the same one that
// writes it), so it's never read half written.
const History& GetHistory();

// Summary of a period with the menu closed.
struct Summary {
    bool valid = false;
    double seconds = 0.0;      // length of the period
    double averageFps = 0.0;   // average FPS (all frames)
    std::uint64_t closedFrames = 0;
    double averageCostUs = 0.0;  // average cost with the menu closed
    double maxCostUs = 0.0;      // max cost with the menu closed
};

// Takes the summary since the last call and resets the accumulators.
// Called from Luma's main thread.
Summary TakeSummary();

// The last summary taken (to show it in the menu).
Summary LastSummary();

// Averages of the last "frames" frames of the history (for the HUD).
// Only from the render thread.
struct Recent {
    double frameMs = 0.0;
    double costUs = 0.0;
};
Recent RecentAverage(std::size_t frames);

// The game's render thread (the one that calls Present). 0 if not seen yet.
std::uint32_t RenderThreadId();

}  // namespace luma::perf
