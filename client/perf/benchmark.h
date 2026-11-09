// =============================================================================
//  benchmark.h - Modo benchmark: medir FPS de forma comparable
// =============================================================================
//  Graba el frametime de CADA frame durante un tiempo fijo y calcula:
//
//    - FPS promedio: frames / segundos.
//    - 1% low: el FPS "de los peores momentos". Se toman el 1% de frames
//      más lentos, se promedia su frametime y se pasa a FPS. Si el promedio
//      es 120 pero el 1% low es 40, el juego da tirones aunque el promedio
//      se vea bien. (0.1% low: lo mismo con el 0,1% peor.)
//    - Frametime promedio, percentil 99 (el 99% de los frames tardó menos
//      que esto) y máximo.
//    - Costo de Luma promedio por frame.
//
//  Y lo exporta a %APPDATA%\Luma\benchmarks\:
//    - <fecha>_<nombre>.csv : un renglón por frame (para graficar).
//    - resumen.csv          : un renglón por prueba (para comparar).
//  Los CSV usan el separador y la coma decimal de TU Windows, así Excel los
//  abre bien a la primera.
//
//  Para que los números sean comparables:
//    - La grabación empieza tras una cuenta regresiva (para que cierres el
//      menú y no haya avisos en pantalla, que cuestan).
//    - Si abres el menú durante la prueba, se cancela.
//    - Modo "Luma en reposo": durante la prueba Luma no dibuja nada (ni el
//      HUD). Su costo baja a ~0,001 ms por frame: es lo más parecido a "sin
//      Luma" que se puede medir desde dentro del juego.
//
//  HILOS: OnFrame en el hilo de render; Poll en el hilo principal de Luma;
//  el resto desde el menú.
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
    std::string mode;            // "con HUD" / "en reposo"
    std::string optimizations;   // optimizaciones activas durante la prueba
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

// Empieza una prueba de "seconds" segundos tras "countdown" segundos.
// Devuelve false si ya hay una en curso.
bool Start(double seconds, double countdown, std::string label, Mode mode, std::string optimizations);
void Cancel(const char* reason);

State GetState();
// Segundos que faltan (de la cuenta regresiva o de la grabación).
double SecondsLeft();

// ¿Luma debe dejar de dibujar? (modo "en reposo" durante la prueba). Barato:
// una lectura atómica, se consulta en el camino rápido del overlay.
bool SuppressDrawing();

// Un frame (hilo de render, desde perf::RecordFrame). No pide memoria.
void OnFrame(std::int64_t frameStart, float frameMs, float costUs);

// Hilo principal de Luma, cada 50 ms: termina las pruebas (cálculos y CSV
// fuera del hilo de render) y cancela si se abrió el menú.
void Poll();

// Las pruebas de esta sesión (la más nueva al final).
std::vector<Result> Results();

}  // namespace luma::perf::benchmark
