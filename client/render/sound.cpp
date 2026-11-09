// =============================================================================
//  sound.cpp - Síntesis y reproducción de los efectos de sonido
// =============================================================================
//  Un sonido digital es una lista de números ("muestras"): la posición de la
//  membrana del parlante, 44 100 veces por segundo. Un tono puro es una onda
//  seno: sin(2π · frecuencia · tiempo). Para que suene agradable y no como un
//  pitido, le damos una "envolvente": el volumen sube rápido al inicio y cae
//  suavemente al final.
//
//  Cada efecto se guarda como un archivo WAV completo en memoria (cabecera +
//  muestras), que es el formato que entiende PlaySound.
// =============================================================================
#include "render/sound.h"

#include <windows.h>
#include <mmsystem.h>  // PlaySound

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "render/theme.h"

namespace luma::sound {

namespace {

constexpr int kSampleRate = 44100;
constexpr int kEffectCount = 6;
constexpr double kPi = 3.14159265358979323846;

std::array<std::vector<std::uint8_t>, kEffectCount> g_wavs;  // un WAV por efecto
std::mutex g_wavMutex;  // protege g_wavs mientras se regeneran
std::thread g_thread;
HANDLE g_wake = nullptr;  // "hay un sonido para reproducir"
std::atomic<int> g_pending{-1};
std::atomic<bool> g_running{false};
float g_volume = 0.6f;

// Un "tono": frecuencia inicial y final (Hz), duración (s), volumen relativo.
struct Tone {
    double startHz;
    double endHz;
    double seconds;
    double gain;
    double delay = 0.0;  // empieza más tarde (para hacer acordes de dos notas)
};

// Escribe un archivo WAV (PCM de 16 bits, mono) en memoria.
std::vector<std::uint8_t> MakeWav(const std::vector<std::int16_t>& samples) {
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    std::vector<std::uint8_t> wav(44 + dataSize);
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + dataSize);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);                 // tamaño del bloque "fmt"
    put16(20, 1);                  // formato PCM
    put16(22, 1);                  // 1 canal
    put32(24, kSampleRate);
    put32(28, kSampleRate * 2);    // bytes por segundo
    put16(32, 2);                  // bytes por muestra
    put16(34, 16);                 // bits por muestra
    std::memcpy(&wav[36], "data", 4);
    put32(40, dataSize);
    std::memcpy(&wav[44], samples.data(), dataSize);
    return wav;
}

// Suma varios tonos en una sola onda.
std::vector<std::uint8_t> Synthesize(std::initializer_list<Tone> tones, float volume) {
    double total = 0.0;
    for (const Tone& t : tones) {
        total = std::max(total, t.delay + t.seconds);
    }
    std::vector<double> mix(static_cast<std::size_t>(total * kSampleRate) + 1, 0.0);
    for (const Tone& t : tones) {
        const std::size_t first = static_cast<std::size_t>(t.delay * kSampleRate);
        const std::size_t count = static_cast<std::size_t>(t.seconds * kSampleRate);
        double phase = 0.0;
        for (std::size_t i = 0; i < count && first + i < mix.size(); ++i) {
            const double progress = static_cast<double>(i) / static_cast<double>(count);
            // La frecuencia se desliza de startHz a endHz (da el efecto "subida").
            const double hz = t.startHz + (t.endHz - t.startHz) * progress;
            phase += 2.0 * kPi * hz / kSampleRate;
            // Envolvente: sube en 4 ms y cae como una curva exponencial.
            const double attack = std::min(1.0, static_cast<double>(i) / (0.004 * kSampleRate));
            const double decay = std::exp(-5.0 * progress);
            // Un poco de segundo armónico para que suene más "redondo".
            const double wave = std::sin(phase) + 0.18 * std::sin(phase * 2.0);
            mix[first + i] += wave * attack * decay * t.gain;
        }
    }
    std::vector<std::int16_t> samples(mix.size());
    for (std::size_t i = 0; i < mix.size(); ++i) {
        const double value = std::clamp(mix[i] * volume, -1.0, 1.0);
        samples[i] = static_cast<std::int16_t>(value * 32000.0);
    }
    return MakeWav(samples);
}

void Generate(float volume) {
    std::array<std::vector<std::uint8_t>, kEffectCount> wavs;
    // Abrir: dos notas ascendentes (una quinta), suaves.
    wavs[static_cast<int>(Effect::Open)] =
        Synthesize({{520, 560, 0.16, 0.30}, {780, 820, 0.18, 0.25, 0.05}}, volume);
    // Cerrar: lo mismo, descendente.
    wavs[static_cast<int>(Effect::Close)] =
        Synthesize({{760, 700, 0.14, 0.28}, {520, 480, 0.16, 0.24, 0.04}}, volume);
    // Clic: un "tic" corto y agudo.
    wavs[static_cast<int>(Effect::Click)] = Synthesize({{1900, 1500, 0.035, 0.35}}, volume);
    // Interruptor: sube (encendido) o baja (apagado).
    wavs[static_cast<int>(Effect::ToggleOn)] =
        Synthesize({{660, 700, 0.07, 0.32}, {990, 1050, 0.09, 0.26, 0.045}}, volume);
    wavs[static_cast<int>(Effect::ToggleOff)] =
        Synthesize({{880, 840, 0.07, 0.30}, {590, 560, 0.09, 0.26, 0.045}}, volume);
    // Pasar el mouse: casi imperceptible.
    wavs[static_cast<int>(Effect::Hover)] = Synthesize({{2600, 2400, 0.018, 0.10}}, volume);

    std::lock_guard lock(g_wavMutex);
    PlaySoundW(nullptr, nullptr, 0);  // detener cualquier sonido antes de cambiar los datos
    g_wavs = std::move(wavs);
}

// Hilo de audio: duerme hasta que se pide un sonido y lo reproduce.
void AudioThread() {
    while (g_running.load()) {
        WaitForSingleObject(g_wake, INFINITE);
        const int effect = g_pending.exchange(-1);
        if (!g_running.load() || effect < 0) {
            continue;
        }
        std::lock_guard lock(g_wavMutex);
        const std::vector<std::uint8_t>& wav = g_wavs[static_cast<std::size_t>(effect)];
        if (!wav.empty()) {
            // SND_MEMORY: los datos están en memoria. SND_ASYNC: no esperar a
            // que termine. SND_NODEFAULT: si algo falla, no sonar el "ding"
            // de Windows.
            PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr,
                       SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
    }
}

}  // namespace

void Init() {
    g_volume = theme::Get().soundVolume;
    Generate(g_volume);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_running.store(true);
    g_thread = std::thread(AudioThread);
}

void Play(Effect effect) {
    if (!g_running.load() || !theme::Get().sounds) {
        return;
    }
    g_pending.store(static_cast<int>(effect));
    SetEvent(g_wake);
}

void SetVolume(float volume) {
    if (volume == g_volume) {
        return;
    }
    g_volume = volume;
    Generate(volume);
}

void Shutdown() {
    if (!g_running.exchange(false)) {
        return;
    }
    SetEvent(g_wake);
    if (g_thread.joinable()) {
        g_thread.join();
    }
    {
        std::lock_guard lock(g_wavMutex);
        PlaySoundW(nullptr, nullptr, 0);  // cortar el sonido que esté sonando
    }
    CloseHandle(g_wake);
    g_wake = nullptr;
}

}  // namespace luma::sound
