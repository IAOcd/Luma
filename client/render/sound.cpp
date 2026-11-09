// =============================================================================
//  sound.cpp - Síntesis y reproducción de los efectos de sonido (XAudio2)
// =============================================================================
//  Un sonido digital es una lista de números ("muestras"): la posición de la
//  membrana del parlante, 44 100 veces por segundo.
//
//  SÍNTESIS "DE VIDRIO": una copa de cristal no suena como un tono puro, sino
//  como una mezcla de frecuencias que NO son múltiplos exactos de la
//  principal (parciales "inarmónicos", aquí x2.76 y x5.40). Cada parcial se
//  apaga a su ritmo (los agudos antes). Encima agregamos un eco muy corto
//  (dos "líneas de retardo"), que da sensación de espacio.
//
//  REPRODUCCIÓN: XAudio2 (viene con Windows 10/11). A diferencia de
//  PlaySound, permite varios sonidos a la vez sin cortarse y con muy poca
//  latencia. Tenemos 8 "voces": cada sonido usa la primera libre.
//
//  Todo corre en un hilo propio de Luma: el render del juego nunca espera al
//  audio.
// =============================================================================
#include "render/sound.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "core/logger.h"
#include "render/theme.h"

namespace luma::sound {

namespace {

constexpr int kSampleRate = 44100;
constexpr int kEffectCount = 6;
constexpr int kVoiceCount = 8;
constexpr double kPi = 3.14159265358979323846;

std::array<std::vector<std::int16_t>, kEffectCount> g_samples;  // un sonido por efecto
std::mutex g_samplesMutex;

IXAudio2* g_engine = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
std::array<IXAudio2SourceVoice*, kVoiceCount> g_voices{};

std::thread g_thread;
HANDLE g_wake = nullptr;
// Pedidos pendientes: un bit por efecto (así varios pedidos seguidos no se pierden).
std::atomic<std::uint32_t> g_pending{0};
std::atomic<bool> g_running{false};
float g_volume = 0.6f;

// Una "nota de vidrio": frecuencia, duración, volumen, retraso y un desliz
// de afinación (para que suba o baje un poco).
struct Note {
    double hz;
    double seconds;
    double gain;
    double delay = 0.0;
    double glide = 0.0;  // fracción: +0.05 = sube 5% durante la nota
};

std::vector<std::int16_t> Synthesize(std::initializer_list<Note> notes, float volume, double echo = 0.18) {
    double total = 0.0;
    for (const Note& n : notes) {
        total = std::max(total, n.delay + n.seconds);
    }
    total += 0.09;  // espacio para la cola del eco
    std::vector<double> mix(static_cast<std::size_t>(total * kSampleRate), 0.0);

    // Parciales de una campana/copa: (multiplicador, volumen, rapidez con que se apaga).
    constexpr double kPartials[3][3] = {{1.0, 1.0, 6.0}, {2.76, 0.32, 11.0}, {5.40, 0.12, 19.0}};
    for (const Note& n : notes) {
        const std::size_t first = static_cast<std::size_t>(n.delay * kSampleRate);
        const std::size_t count = static_cast<std::size_t>(n.seconds * kSampleRate);
        double phase[3] = {0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < count && first + i < mix.size(); ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            const double progress = static_cast<double>(i) / static_cast<double>(count);
            const double hz = n.hz * (1.0 + n.glide * progress);
            // Ataque suave de 6 ms para que no haga "clic" al empezar.
            const double attack = std::min(1.0, t / 0.006);
            double sample = 0.0;
            for (int k = 0; k < 3; ++k) {
                phase[k] += 2.0 * kPi * hz * kPartials[k][0] / kSampleRate;
                sample += std::sin(phase[k]) * kPartials[k][1] * std::exp(-kPartials[k][2] * t / n.seconds);
            }
            // Desvanecido final para no cortar en seco.
            const double release = std::min(1.0, (1.0 - progress) / 0.15);
            mix[first + i] += sample * attack * release * n.gain;
        }
    }

    // Eco corto: dos retardos (23 ms y 37 ms) mezclados suave.
    if (echo > 0.0) {
        const std::size_t d1 = static_cast<std::size_t>(0.023 * kSampleRate);
        const std::size_t d2 = static_cast<std::size_t>(0.037 * kSampleRate);
        std::vector<double> wet(mix.size(), 0.0);
        for (std::size_t i = 0; i < mix.size(); ++i) {
            if (i >= d1) wet[i] += mix[i - d1] * 0.6 + wet[i - d1] * 0.25;
            if (i >= d2) wet[i] += mix[i - d2] * 0.4;
        }
        for (std::size_t i = 0; i < mix.size(); ++i) {
            mix[i] += wet[i] * echo;
        }
    }

    std::vector<std::int16_t> out(mix.size());
    for (std::size_t i = 0; i < mix.size(); ++i) {
        // tanh "redondea" los picos: si dos notas suman mucho, no distorsiona feo.
        const double value = std::tanh(mix[i] * volume * 0.9);
        out[i] = static_cast<std::int16_t>(value * 30000.0);
    }
    return out;
}

void Generate(float volume) {
    std::array<std::vector<std::int16_t>, kEffectCount> s;
    // Abrir: dos notas de vidrio que suben (Do5 -> Sol5), con un leve desliz.
    s[static_cast<int>(sound::Effect::Open)] =
        Synthesize({{523.25, 0.32, 0.30, 0.0, 0.01}, {783.99, 0.42, 0.26, 0.055, 0.01}}, volume);
    // Cerrar: dos notas que bajan, más cortas.
    s[static_cast<int>(sound::Effect::Close)] =
        Synthesize({{698.46, 0.22, 0.26, 0.0, -0.02}, {466.16, 0.30, 0.24, 0.045, -0.02}}, volume);
    // Clic: una gotita de cristal, muy breve.
    s[static_cast<int>(sound::Effect::Click)] = Synthesize({{1567.98, 0.07, 0.22, 0.0, -0.04}}, volume, 0.08);
    // Interruptor: encender sube (quinta), apagar baja.
    s[static_cast<int>(sound::Effect::ToggleOn)] =
        Synthesize({{880.0, 0.13, 0.22, 0.0, 0.03}, {1318.5, 0.18, 0.18, 0.04, 0.0}}, volume, 0.12);
    s[static_cast<int>(sound::Effect::ToggleOff)] =
        Synthesize({{987.77, 0.12, 0.20, 0.0, -0.03}, {659.25, 0.16, 0.18, 0.04, 0.0}}, volume, 0.12);
    // Pasar el mouse: una "burbuja" casi imperceptible.
    s[static_cast<int>(sound::Effect::Hover)] = Synthesize({{1975.5, 0.035, 0.06, 0.0, 0.08}}, volume, 0.0);

    std::lock_guard lock(g_samplesMutex);
    // Detener las voces antes de cambiar los datos que podrían estar sonando.
    for (IXAudio2SourceVoice* voice : g_voices) {
        if (voice != nullptr) {
            voice->Stop();
            voice->FlushSourceBuffers();
        }
    }
    g_samples = std::move(s);
}

bool InitEngine() {
    if (FAILED(XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        log::Warn("Sonidos: no se pudo iniciar XAudio2.");
        return false;
    }
    if (FAILED(g_engine->CreateMasteringVoice(&g_master))) {
        log::Warn("Sonidos: no se pudo abrir la salida de audio.");
        return false;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = kSampleRate * 2;
    for (IXAudio2SourceVoice*& voice : g_voices) {
        if (FAILED(g_engine->CreateSourceVoice(&voice, &format))) {
            voice = nullptr;
        }
    }
    return true;
}

void ShutdownEngine() {
    for (IXAudio2SourceVoice*& voice : g_voices) {
        if (voice != nullptr) {
            voice->DestroyVoice();
            voice = nullptr;
        }
    }
    if (g_master != nullptr) {
        g_master->DestroyVoice();
        g_master = nullptr;
    }
    if (g_engine != nullptr) {
        g_engine->Release();
        g_engine = nullptr;
    }
}

void PlayNow(int effect) {
    std::lock_guard lock(g_samplesMutex);
    const std::vector<std::int16_t>& samples = g_samples[static_cast<std::size_t>(effect)];
    if (samples.empty()) {
        return;
    }
    // Buscar una voz que no esté sonando.
    for (IXAudio2SourceVoice* voice : g_voices) {
        if (voice == nullptr) {
            continue;
        }
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued > 0) {
            continue;
        }
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(samples.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(samples.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        voice->SubmitSourceBuffer(&buffer);
        voice->Start();
        return;
    }
    // Todas ocupadas: este sonido se omite (son muy cortos, no se nota).
}

// Hilo de audio: arranca XAudio2, duerme hasta que se pide un sonido y lo
// reproduce. COM (que XAudio2 usa por dentro) se inicia en ESTE hilo.
void AudioThread() {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = InitEngine();
    while (g_running.load()) {
        WaitForSingleObject(g_wake, INFINITE);
        const std::uint32_t pending = g_pending.exchange(0);
        if (!ok || !g_running.load()) {
            continue;
        }
        for (int effect = 0; effect < kEffectCount; ++effect) {
            if (pending & (1u << effect)) {
                PlayNow(effect);
            }
        }
    }
    ShutdownEngine();
    if (SUCCEEDED(comInit)) {
        CoUninitialize();
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
    g_pending.fetch_or(1u << static_cast<int>(effect));
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
        g_thread.join();  // el hilo destruye las voces y XAudio2 al salir
    }
    CloseHandle(g_wake);
    g_wake = nullptr;
}

}  // namespace luma::sound
