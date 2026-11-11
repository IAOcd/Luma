// =============================================================================
//  benchmark.cpp - Benchmark mode implementation
// =============================================================================
#include "perf/benchmark.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>

#include "core/logger.h"
#include "core/paths.h"
#include "perf/gpu_timer.h"
#include "perf/timer.h"
#include "render/notifications.h"
#include "render/overlay.h"

namespace luma::perf::benchmark {

namespace {

// Cap on frames per second we can store (plenty: 2000 FPS).
constexpr double kMaxFps = 2000.0;
constexpr double kMaxSeconds = 600.0;

std::atomic<State> g_state{State::Idle};
std::atomic<bool> g_suppress{false};

// The test's data. The memory is reserved when it STARTS (on the menu's
// thread); the render thread only writes into what's already reserved.
std::vector<float> g_frameMs;
std::vector<float> g_costUs;
std::vector<float> g_gpuMs;  // GPU busy time of a recent frame (negative = no measurement)
std::atomic<std::size_t> g_count{0};
std::atomic<bool> g_writing{false};
std::atomic<std::int64_t> g_recordStart{0};  // when recording starts (ticks)
std::atomic<std::int64_t> g_recordEnd{0};    // when it ends

// Data of the test in progress (only the menu and the main thread touch it).
std::mutex g_mutex;
std::string g_label;
std::string g_optimizations;
Mode g_mode = Mode::WithHud;
double g_seconds = 0.0;
std::vector<Result> g_results;

std::string NowText(const char* format) {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), format, &local);
    return buffer;
}

// List separator and decimal separator of the user's Windows (the ones
// Excel uses). In Spanish they're usually ";" and ",".
struct CsvFormat {
    char list = ',';
    char decimal = '.';
};

CsvFormat UserCsvFormat() {
    CsvFormat f;
    wchar_t buffer[8] = L"";
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buffer, 8) > 0 && buffer[0] < 128) {
        f.decimal = static_cast<char>(buffer[0]);
    }
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLIST, buffer, 8) > 0 && buffer[0] < 128) {
        f.list = static_cast<char>(buffer[0]);
    }
    if (f.list == f.decimal) {
        f.list = ';';
    }
    return f;
}

std::string Num(double value, int decimals, const CsvFormat& f) {
    std::string s = std::format("{:.{}f}", value, decimals);
    if (f.decimal != '.') {
        std::replace(s.begin(), s.end(), '.', f.decimal);
    }
    return s;
}

// A text for a file name: only letters, numbers, '-' and '_'.
std::string SafeFileName(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ' && !out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    return out.empty() ? "prueba" : out.substr(0, 40);
}

// A text for the CSV: in quotes if it has the separator or quotes.
std::string CsvText(const std::string& text, const CsvFormat& f) {
    if (text.find(f.list) == std::string::npos && text.find('"') == std::string::npos) {
        return text;
    }
    std::string out = "\"";
    for (char c : text) {
        out += c == '"' ? "\"\"" : std::string(1, c);
    }
    return out + "\"";
}

Result Compute(std::size_t count) {
    Result r;
    if (count == 0) {
        return r;
    }
    std::vector<float> sorted(g_frameMs.begin(), g_frameMs.begin() + static_cast<std::ptrdiff_t>(count));
    double totalMs = 0.0;
    double costSum = 0.0;
    double gpuSum = 0.0;
    std::size_t gpuFrames = 0;
    for (std::size_t i = 0; i < count; ++i) {
        totalMs += g_frameMs[i];
        costSum += g_costUs[i];
        if (g_gpuMs[i] >= 0.0f) {
            gpuSum += g_gpuMs[i];
            ++gpuFrames;
        }
    }
    std::sort(sorted.begin(), sorted.end());
    auto worstAverage = [&](std::size_t n) {
        n = std::max<std::size_t>(1, n);
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            sum += sorted[count - 1 - i];
        }
        return sum / static_cast<double>(n);
    };
    r.frames = count;
    r.seconds = totalMs / 1000.0;
    r.averageMs = totalMs / static_cast<double>(count);
    r.averageFps = r.averageMs > 0.0 ? 1000.0 / r.averageMs : 0.0;
    const double worst1 = worstAverage(count / 100);
    const double worst01 = worstAverage(count / 1000);
    r.low1Fps = worst1 > 0.0 ? 1000.0 / worst1 : 0.0;
    r.low01Fps = worst01 > 0.0 ? 1000.0 / worst01 : 0.0;
    r.p99Ms = sorted[std::min(count - 1, (count * 99) / 100)];
    r.maxMs = sorted[count - 1];
    r.lumaCostMs = costSum / static_cast<double>(count) / 1000.0;
    // The GPU numbers only count if most frames were measured.
    if (gpuFrames * 2 >= count) {
        r.gpuMs = gpuSum / static_cast<double>(gpuFrames);
        r.gpuPercent = r.averageMs > 0.0 ? std::min(100.0, r.gpuMs / r.averageMs * 100.0) : 0.0;
    }
    return r;
}

void WriteFiles(Result& r, std::size_t count) {
    std::error_code ec;
    const std::filesystem::path dir = paths::DataDir() / L"benchmarks";
    std::filesystem::create_directories(dir, ec);
    const CsvFormat f = UserCsvFormat();
    const char sep = f.list;
    const char* bom = "\xEF\xBB\xBF";  // UTF-8 mark: so Excel shows accented characters correctly

    // 1. One row per frame.
    const std::filesystem::path framesPath =
        dir / std::filesystem::path(paths::ToUtf8(std::filesystem::path(NowText("%Y-%m-%d_%H-%M-%S"))) + "_" +
                                    SafeFileName(r.label) + ".csv");
    {
        std::ofstream out(framesPath, std::ios::binary);
        out << bom << "frame" << sep << "time_s" << sep << "frametime_ms" << sep << "fps" << sep << "luma_cost_ms" << sep
            << "gpu_ms\n";
        double elapsed = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            elapsed += g_frameMs[i] / 1000.0;
            const double fps = g_frameMs[i] > 0.0f ? 1000.0 / g_frameMs[i] : 0.0;
            out << (i + 1) << sep << Num(elapsed, 4, f) << sep << Num(g_frameMs[i], 3, f) << sep << Num(fps, 1, f)
                << sep << Num(g_costUs[i] / 1000.0, 4, f) << sep
                << (g_gpuMs[i] >= 0.0f ? Num(g_gpuMs[i], 3, f) : std::string()) << '\n';
        }
        r.csvPath = paths::ToUtf8(framesPath);
        if (!out) {
            log::Warn("Benchmark: couldn't write {}.", r.csvPath);
        }
    }

    // 2. One line in the summary (with a header if the file is new).
    const std::filesystem::path summaryPath = dir / L"summary.csv";
    const bool isNew = !std::filesystem::exists(summaryPath, ec);
    std::ofstream out(summaryPath, std::ios::binary | std::ios::app);
    if (isNew) {
        out << bom << "date" << sep << "name" << sep << "mode" << sep << "optimizations" << sep << "seconds" << sep
            << "frames" << sep << "average_fps" << sep << "1%_low" << sep << "0.1%_low" << sep << "frametime_ms" << sep
            << "p99_ms" << sep << "max_ms" << sep << "luma_cost_ms" << sep << "gpu_ms" << sep << "gpu_percent\n";
    }
    out << r.date << sep << CsvText(r.label, f) << sep << r.mode << sep << CsvText(r.optimizations, f) << sep
        << Num(r.seconds, 1, f) << sep << r.frames << sep << Num(r.averageFps, 1, f) << sep << Num(r.low1Fps, 1, f)
        << sep << Num(r.low01Fps, 1, f) << sep << Num(r.averageMs, 3, f) << sep << Num(r.p99Ms, 3, f) << sep
        << Num(r.maxMs, 3, f) << sep << Num(r.lumaCostMs, 4, f) << sep
        << (r.gpuMs >= 0.0 ? Num(r.gpuMs, 3, f) : std::string()) << sep
        << (r.gpuMs >= 0.0 ? Num(r.gpuPercent, 1, f) : std::string()) << '\n';
}

void Finish() {
    const std::size_t count = g_count.load();
    Result r = Compute(count);
    {
        std::lock_guard lock(g_mutex);
        r.label = g_label;
        r.optimizations = g_optimizations.empty() ? "none" : g_optimizations;
        r.mode = g_mode == Mode::Resting ? "idle" : "with HUD";
    }
    r.date = NowText("%Y-%m-%d %H:%M:%S");
    if (count < 10) {
        notify::Push(notify::Kind::Warning, "Benchmark: too few frames, not saved");
    } else {
        WriteFiles(r, count);
        log::Info("Benchmark \"{}\" ({}): {:.1f} FPS promedio, 1% low {:.1f}, 0.1% low {:.1f}, frametime {:.3f} ms, "
                  "Luma cost {:.4f} ms, GPU {:.2f} ms ({:.0f}% of the frame). CSV: {}",
                  r.label, r.mode, r.averageFps, r.low1Fps, r.low01Fps, r.averageMs, r.lumaCostMs, r.gpuMs,
                  r.gpuPercent, r.csvPath);
        notify::Push(notify::Kind::Success,
                     std::format("Benchmark done: {:.0f} FPS · 1% low {:.0f}", r.averageFps, r.low1Fps));
        std::lock_guard lock(g_mutex);
        g_results.push_back(std::move(r));
    }
    // Free the test's memory.
    std::vector<float>().swap(g_frameMs);
    std::vector<float>().swap(g_costUs);
    std::vector<float>().swap(g_gpuMs);
    gputimer::Want(gputimer::kBenchmark, false);
    g_suppress.store(false);
    g_state.store(State::Idle);
}

}  // namespace

bool Start(double seconds, double countdown, std::string label, Mode mode, std::string optimizations) {
    if (g_state.load() != State::Idle) {
        return false;
    }
    while (g_writing.load()) {
        YieldProcessor();  // the render thread is finishing writing a frame
    }
    seconds = std::clamp(seconds, 5.0, kMaxSeconds);
    const auto capacity = static_cast<std::size_t>(seconds * kMaxFps);
    // Reserve BEFORE the render thread starts writing.
    g_frameMs.assign(capacity, 0.0f);
    g_costUs.assign(capacity, 0.0f);
    g_gpuMs.assign(capacity, -1.0f);
    g_count.store(0);
    {
        std::lock_guard lock(g_mutex);
        g_label = label.empty() ? std::format("Test {}", g_results.size() + 1) : std::move(label);
        g_optimizations = std::move(optimizations);
        g_mode = mode;
        g_seconds = seconds;
    }
    const std::int64_t now = Now();
    const std::int64_t start = now + static_cast<std::int64_t>(countdown * static_cast<double>(g_ticksPerSecond));
    g_recordStart.store(start);
    g_recordEnd.store(start + static_cast<std::int64_t>(seconds * static_cast<double>(g_ticksPerSecond)));
    g_suppress.store(mode == Mode::Resting);
    // Measure the GPU too (it starts during the countdown, so it's warm when
    // the recording begins).
    gputimer::Want(gputimer::kBenchmark, true);
    g_state.store(State::Countdown);
    log::Info("Benchmark: starts in {:.0f} s and lasts {:.0f} s (mode {}).", countdown, seconds,
              mode == Mode::Resting ? "idle" : "with HUD");
    return true;
}

void Cancel(const char* reason) {
    const State state = g_state.load();
    if (state != State::Countdown && state != State::Recording) {
        return;
    }
    g_state.store(State::Idle);
    g_suppress.store(false);
    gputimer::Want(gputimer::kBenchmark, false);
    log::Info("Benchmark canceled: {}.", reason);
    notify::Push(notify::Kind::Warning, std::format("Benchmark canceled: {}", reason));
}

State GetState() {
    return g_state.load(std::memory_order_relaxed);
}

double SecondsLeft() {
    const State state = g_state.load();
    const std::int64_t target = state == State::Countdown ? g_recordStart.load() : g_recordEnd.load();
    if (state != State::Countdown && state != State::Recording) {
        return 0.0;
    }
    return std::max(0.0, TicksToMs(target - Now()) / 1000.0);
}

bool SuppressDrawing() {
    return g_suppress.load(std::memory_order_relaxed);
}

void OnFrame(std::int64_t frameStart, float frameMs, float costUs) {
    const State state = g_state.load(std::memory_order_acquire);
    if (state == State::Countdown) {
        if (frameStart < g_recordStart.load(std::memory_order_relaxed)) {
            return;
        }
        g_state.store(State::Recording);
        return;  // the recording's first frame starts counting from here
    }
    if (state != State::Recording) {
        return;
    }
    // "I'm writing": Start waits for it to finish before changing the
    // arrays (in case a test is canceled and another one starts right now).
    // First it's marked and THEN the state is checked again: that way it never
    // writes into an array another thread is replacing.
    g_writing.store(true);
    if (g_state.load() == State::Recording) {
        const std::size_t i = g_count.load(std::memory_order_relaxed);
        if (frameStart >= g_recordEnd.load(std::memory_order_relaxed) || i >= g_frameMs.size()) {
            g_state.store(State::Saving);
        } else {
            g_frameMs[i] = frameMs;
            g_costUs[i] = costUs;
            g_gpuMs[i] = gputimer::LastFrameMs();
            g_count.store(i + 1, std::memory_order_release);
        }
    }
    g_writing.store(false);
}

void Poll() {
    const State state = g_state.load();
    if ((state == State::Countdown || state == State::Recording) && render::overlay::IsMenuOpen()) {
        // Opening the menu is allowed during the countdown, not while recording.
        if (state == State::Recording) {
            Cancel("the menu was opened");
        }
        return;
    }
    if (state == State::Saving) {
        Finish();
    }
}

std::vector<Result> Results() {
    std::lock_guard lock(g_mutex);
    return g_results;
}

}  // namespace luma::perf::benchmark
