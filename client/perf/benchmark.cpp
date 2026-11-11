// =============================================================================
//  benchmark.cpp - Implementación del modo benchmark
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
#include "perf/timer.h"
#include "render/notifications.h"
#include "render/overlay.h"

namespace luma::perf::benchmark {

namespace {

// Tope de frames por segundo que podemos guardar (de sobra: 2000 FPS).
constexpr double kMaxFps = 2000.0;
constexpr double kMaxSeconds = 600.0;

std::atomic<State> g_state{State::Idle};
std::atomic<bool> g_suppress{false};

// Los datos de la prueba. Se reserva la memoria al EMPEZAR (en el hilo del
// menú); el hilo de render solo escribe en lo ya reservado.
std::vector<float> g_frameMs;
std::vector<float> g_costUs;
std::atomic<std::size_t> g_count{0};
std::atomic<bool> g_writing{false};
std::atomic<std::int64_t> g_recordStart{0};  // cuándo empieza a grabar (ticks)
std::atomic<std::int64_t> g_recordEnd{0};    // cuándo termina

// Datos de la prueba en curso (los toca solo el menú y el hilo principal).
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

// Separador de listas y separador decimal del Windows del usuario (los que
// usa Excel). En español suele ser ";" y ",".
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

// Un texto para un nombre de archivo: solo letras, números, '-' y '_'.
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

// Un texto para el CSV: entre comillas si tiene el separador o comillas.
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
    for (std::size_t i = 0; i < count; ++i) {
        totalMs += g_frameMs[i];
        costSum += g_costUs[i];
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
    return r;
}

void WriteFiles(Result& r, std::size_t count) {
    std::error_code ec;
    const std::filesystem::path dir = paths::DataDir() / L"benchmarks";
    std::filesystem::create_directories(dir, ec);
    const CsvFormat f = UserCsvFormat();
    const char sep = f.list;
    const char* bom = "\xEF\xBB\xBF";  // marca UTF-8: así Excel muestra bien las tildes

    // 1. Un renglón por frame.
    const std::filesystem::path framesPath =
        dir / std::filesystem::path(paths::ToUtf8(std::filesystem::path(NowText("%Y-%m-%d_%H-%M-%S"))) + "_" +
                                    SafeFileName(r.label) + ".csv");
    {
        std::ofstream out(framesPath, std::ios::binary);
        out << bom << "frame" << sep << "tiempo_s" << sep << "frametime_ms" << sep << "fps" << sep << "costo_luma_ms\n";
        double elapsed = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            elapsed += g_frameMs[i] / 1000.0;
            const double fps = g_frameMs[i] > 0.0f ? 1000.0 / g_frameMs[i] : 0.0;
            out << (i + 1) << sep << Num(elapsed, 4, f) << sep << Num(g_frameMs[i], 3, f) << sep << Num(fps, 1, f)
                << sep << Num(g_costUs[i] / 1000.0, 4, f) << '\n';
        }
        r.csvPath = paths::ToUtf8(framesPath);
        if (!out) {
            log::Warn("Benchmark: no se pudo escribir {}.", r.csvPath);
        }
    }

    // 2. Una línea en el resumen (con encabezado si el archivo es nuevo).
    const std::filesystem::path summaryPath = dir / L"resumen.csv";
    const bool isNew = !std::filesystem::exists(summaryPath, ec);
    std::ofstream out(summaryPath, std::ios::binary | std::ios::app);
    if (isNew) {
        out << bom << "fecha" << sep << "nombre" << sep << "modo" << sep << "optimizaciones" << sep << "segundos" << sep
            << "frames" << sep << "fps_promedio" << sep << "1%_low" << sep << "0.1%_low" << sep << "frametime_ms" << sep
            << "p99_ms" << sep << "max_ms" << sep << "costo_luma_ms\n";
    }
    out << r.date << sep << CsvText(r.label, f) << sep << r.mode << sep << CsvText(r.optimizations, f) << sep
        << Num(r.seconds, 1, f) << sep << r.frames << sep << Num(r.averageFps, 1, f) << sep << Num(r.low1Fps, 1, f)
        << sep << Num(r.low01Fps, 1, f) << sep << Num(r.averageMs, 3, f) << sep << Num(r.p99Ms, 3, f) << sep
        << Num(r.maxMs, 3, f) << sep << Num(r.lumaCostMs, 4, f) << '\n';
}

void Finish() {
    const std::size_t count = g_count.load();
    Result r = Compute(count);
    {
        std::lock_guard lock(g_mutex);
        r.label = g_label;
        r.optimizations = g_optimizations.empty() ? "none" : g_optimizations;
        r.mode = g_mode == Mode::Resting ? "en reposo" : "con HUD";
    }
    r.date = NowText("%Y-%m-%d %H:%M:%S");
    if (count < 10) {
        notify::Push(notify::Kind::Warning, "Benchmark: muy pocos frames, no se guardó");
    } else {
        WriteFiles(r, count);
        log::Info("Benchmark \"{}\" ({}): {:.1f} FPS promedio, 1% low {:.1f}, 0.1% low {:.1f}, frametime {:.3f} ms, "
                  "costo de Luma {:.4f} ms. CSV: {}",
                  r.label, r.mode, r.averageFps, r.low1Fps, r.low01Fps, r.averageMs, r.lumaCostMs, r.csvPath);
        notify::Push(notify::Kind::Success,
                     std::format("Benchmark listo: {:.0f} FPS · 1% low {:.0f}", r.averageFps, r.low1Fps));
        std::lock_guard lock(g_mutex);
        g_results.push_back(std::move(r));
    }
    // Liberar la memoria de la prueba.
    std::vector<float>().swap(g_frameMs);
    std::vector<float>().swap(g_costUs);
    g_suppress.store(false);
    g_state.store(State::Idle);
}

}  // namespace

bool Start(double seconds, double countdown, std::string label, Mode mode, std::string optimizations) {
    if (g_state.load() != State::Idle) {
        return false;
    }
    while (g_writing.load()) {
        YieldProcessor();  // el hilo de render está terminando de escribir un frame
    }
    seconds = std::clamp(seconds, 5.0, kMaxSeconds);
    const auto capacity = static_cast<std::size_t>(seconds * kMaxFps);
    // Reservar ANTES de que el hilo de render empiece a escribir.
    g_frameMs.assign(capacity, 0.0f);
    g_costUs.assign(capacity, 0.0f);
    g_count.store(0);
    {
        std::lock_guard lock(g_mutex);
        g_label = label.empty() ? std::format("Prueba {}", g_results.size() + 1) : std::move(label);
        g_optimizations = std::move(optimizations);
        g_mode = mode;
        g_seconds = seconds;
    }
    const std::int64_t now = Now();
    const std::int64_t start = now + static_cast<std::int64_t>(countdown * static_cast<double>(g_ticksPerSecond));
    g_recordStart.store(start);
    g_recordEnd.store(start + static_cast<std::int64_t>(seconds * static_cast<double>(g_ticksPerSecond)));
    g_suppress.store(mode == Mode::Resting);
    g_state.store(State::Countdown);
    log::Info("Benchmark: empieza en {:.0f} s y dura {:.0f} s (modo {}).", countdown, seconds,
              mode == Mode::Resting ? "en reposo" : "con HUD");
    return true;
}

void Cancel(const char* reason) {
    const State state = g_state.load();
    if (state != State::Countdown && state != State::Recording) {
        return;
    }
    g_state.store(State::Idle);
    g_suppress.store(false);
    log::Info("Benchmark cancelado: {}.", reason);
    notify::Push(notify::Kind::Warning, std::format("Benchmark cancelado: {}", reason));
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
        return;  // el primer frame de la grabación empieza a contar desde aquí
    }
    if (state != State::Recording) {
        return;
    }
    // "Estoy escribiendo": Start espera a que termine antes de cambiar los
    // arreglos (por si una prueba se cancela y empieza otra justo ahora).
    // Primero se marca y DESPUÉS se vuelve a mirar el estado: así nunca se
    // escribe en un arreglo que otro hilo está reemplazando.
    g_writing.store(true);
    if (g_state.load() == State::Recording) {
        const std::size_t i = g_count.load(std::memory_order_relaxed);
        if (frameStart >= g_recordEnd.load(std::memory_order_relaxed) || i >= g_frameMs.size()) {
            g_state.store(State::Saving);
        } else {
            g_frameMs[i] = frameMs;
            g_costUs[i] = costUs;
            g_count.store(i + 1, std::memory_order_release);
        }
    }
    g_writing.store(false);
}

void Poll() {
    const State state = g_state.load();
    if ((state == State::Countdown || state == State::Recording) && render::overlay::IsMenuOpen()) {
        // Se permite abrir el menú durante la cuenta regresiva, no grabando.
        if (state == State::Recording) {
            Cancel("se abrió el menú");
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
