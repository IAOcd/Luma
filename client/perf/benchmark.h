// =============================================================================
//  benchmark.h - Benchmark mode: measuring FPS in a comparable way
// =============================================================================
//  Records the frametime of EVERY frame for a fixed time and calculates:
//
//    - Average FPS: frames / seconds.
//    - 1% low: the FPS "of the worst moments". It takes the slowest 1% of
//      frames, averages their frametime and turns it into FPS. If the average
//      is 120 but the 1% low is 40, the game stutters even though the average
//      looks good. (0.1% low: the same with the worst 0.1%.)
//    - Average frametime, 99th percentile (99% of the frames took less than
//      this) and max.
//    - Average Luma cost per frame.
//
//  And exports it to %APPDATA%\Luma\benchmarks\:
//    - <date>_<name>.csv : one row per frame (to chart).
//    - summary.csv       : one row per test (to compare).
//  The CSVs use YOUR Windows list separator and decimal comma, so Excel
//  opens them right the first time.
//
//  So the numbers are comparable:
//    - Recording starts after a countdown (so you close the menu and there
//      are no toasts on screen, which cost something).
//    - If you open the menu during the test, it's canceled.
//    - "Luma idle" mode: during the test Luma draws nothing (not even the
//      HUD). Its cost drops to ~0.001 ms per frame: it's the closest thing to
//      "without Luma" that can be measured from inside the game.
//
//  THREADS: OnFrame on the render thread; Poll on Luma's main thread;
//  the rest from the menu.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::perf::benchmark {

enum class State { Idle, Countdown, Recording, Saving };
enum class Mode { WithHud = 0, Resting = 1 };

struct Result {
    std::string label;
    std::string date;            // "2026-11-09 15:30:12"
    std::string mode;            // "with HUD" / "idle"
    std::string optimizations;   // optimizations active during the test
    double seconds = 0.0;
    std::uint64_t frames = 0;
    double averageFps = 0.0;
    double low1Fps = 0.0;
    double low01Fps = 0.0;
    double averageMs = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
    double lumaCostMs = 0.0;
    std::string csvPath;
};

// Starts a test of "seconds" seconds after "countdown" seconds.
// Returns false if one is already running.
bool Start(double seconds, double countdown, std::string label, Mode mode, std::string optimizations);
void Cancel(const char* reason);

State GetState();
// Seconds left (of the countdown or of the recording).
double SecondsLeft();

// Should Luma stop drawing? ("idle" mode during the test). Cheap:
// one atomic read, checked on the overlay's fast path.
bool SuppressDrawing();

// One frame (render thread, from perf::RecordFrame). Doesn't allocate memory.
void OnFrame(std::int64_t frameStart, float frameMs, float costUs);

// Luma's main thread, every 50 ms: finishes the tests (calculations and CSV
// outside the render thread) and cancels if the menu was opened.
void Poll();

// This session's tests (the newest last).
std::vector<Result> Results();

}  // namespace luma::perf::benchmark
