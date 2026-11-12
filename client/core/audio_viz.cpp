// =============================================================================
//  audio_viz.cpp - What the music sounds like right now (see audio_viz.h)
// =============================================================================
#include "core/audio_viz.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "core/logger.h"

namespace luma::audioviz {

namespace {

constexpr int kFftSize = 2048;
constexpr float kPi = 3.14159265358979f;

std::atomic<bool> g_wanted{false};
std::atomic<bool> g_stop{false};
std::atomic<float> g_sensitivity{1.0f};
std::atomic<float> g_smoothing{0.5f};
std::thread g_thread;
std::mutex g_threadMutex;

std::mutex g_mutex;
Levels g_levels;

// --- The "process loopback" activation (Windows 10 2004+) -----------------------------
// Windows answers this request "later" (asynchronously): it calls this
// object back when the audio client is ready. We just wait for that.
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() {
        if (done_ != nullptr) CloseHandle(done_);
    }
    HANDLE Done() const { return done_; }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refs_;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

private:
    std::atomic<long> refs_{1};
    HANDLE done_ = nullptr;
};

// The sound format we read, to turn samples into numbers -1..1.
struct Format {
    int channels = 2;
    int bits = 16;
    bool isFloat = false;
    int rate = 44100;
};

struct Capture {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    Format format;
    bool processOnly = false;

    void Release() {
        if (client != nullptr) client->Stop();
        if (capture != nullptr) capture->Release();
        if (client != nullptr) client->Release();
        capture = nullptr;
        client = nullptr;
    }
};

// Everything except Minecraft (this process and its children).
bool OpenProcessLoopback(Capture& c) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    auto* handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op);
    bool ok = false;
    if (SUCCEEDED(hr) && WaitForSingleObject(handler->Done(), 3000) == WAIT_OBJECT_0) {
        HRESULT result = E_FAIL;
        IUnknown* unknown = nullptr;
        if (SUCCEEDED(op->GetActivateResult(&result, &unknown)) && SUCCEEDED(result) && unknown != nullptr) {
            ok = SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&c.client)));
            unknown->Release();
        }
    }
    if (op != nullptr) op->Release();
    handler->Release();
    if (!ok) return false;
    // Process loopback doesn't tell its format: we choose one (16-bit, 44.1 kHz, stereo).
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = static_cast<WORD>(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    hr = c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &wf, nullptr);
    if (FAILED(hr) || FAILED(c.client->GetService(IID_PPV_ARGS(&c.capture))) || FAILED(c.client->Start())) {
        c.Release();
        return false;
    }
    c.format = Format{2, 16, false, 44100};
    c.processOnly = true;
    return true;
}

// All the sound of the default speakers (older Windows).
bool OpenDeviceLoopback(Capture& c) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    WAVEFORMATEX* mix = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
              SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
              SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&c.client))) &&
              SUCCEEDED(c.client->GetMixFormat(&mix)) &&
              SUCCEEDED(c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, nullptr)) &&
              SUCCEEDED(c.client->GetService(IID_PPV_ARGS(&c.capture))) && SUCCEEDED(c.client->Start());
    if (ok) {
        c.format.channels = mix->nChannels;
        c.format.bits = mix->wBitsPerSample;
        c.format.rate = static_cast<int>(mix->nSamplesPerSec);
        c.format.isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                           (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        c.processOnly = false;
    } else {
        c.Release();
    }
    if (mix != nullptr) CoTaskMemFree(mix);
    if (device != nullptr) device->Release();
    if (enumerator != nullptr) enumerator->Release();
    return ok;
}

// --- FFT -------------------------------------------------------------------------------------
// The classic "radix-2" recipe: split the samples in even and odd, again and
// again, and combine them back with turns of the unit circle.
void Fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * kPi / static_cast<float>(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

void ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Capture capture;
    std::vector<float> ring(kFftSize, 0.0f);  // the last 2048 samples (mono)
    std::size_t head = 0;
    std::vector<std::complex<float>> spectrum(kFftSize);
    std::vector<float> window(kFftSize);
    for (int i = 0; i < kFftSize; ++i) window[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / (kFftSize - 1));
    float smooth[kBands]{};
    float fast[kBands]{};
    float peaks[kBands]{};
    float peakAge[kBands]{};
    float bassSmooth = 0.0f, levelSmooth = 0.0f;
    float lastBass = 0.0f, bassAverage = 0.0f, kick = 0.0f, waveScale = 0.1f;
    auto lastFft = std::chrono::steady_clock::now();
    int failures = 0;
    while (!g_stop.load()) {
        if (!g_wanted.load()) {
            if (capture.client != nullptr) {
                capture.Release();
                std::lock_guard lock(g_mutex);
                g_levels = Levels{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (capture.client == nullptr) {
            if (failures > 3) {  // it doesn't work on this PC: try again much later
                std::this_thread::sleep_for(std::chrono::seconds(5));
                failures = 2;
            }
            if (OpenProcessLoopback(capture) || OpenDeviceLoopback(capture)) {
                log::Info("Visualizer: listening to {} ({} Hz).", capture.processOnly ? "everything except Minecraft" : "all the sound",
                          capture.format.rate);
                failures = 0;
            } else {
                ++failures;
                log::Warn("Visualizer: couldn't listen to the sound.");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }
        // Read what arrived.
        UINT32 packet = 0;
        bool broken = false;
        while (SUCCEEDED(capture.capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                broken = true;
                break;
            }
            const Format& f = capture.format;
            const int bytes = f.bits / 8;
            for (UINT32 i = 0; i < frames; ++i) {
                float mono = 0.0f;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data != nullptr) {
                    for (int ch = 0; ch < std::min(f.channels, 2); ++ch) {
                        const BYTE* p = data + (static_cast<std::size_t>(i) * f.channels + ch) * bytes;
                        float v = 0.0f;
                        if (f.isFloat && bytes == 4) {
                            std::memcpy(&v, p, 4);
                        } else if (bytes == 2) {
                            std::int16_t s16;
                            std::memcpy(&s16, p, 2);
                            v = static_cast<float>(s16) / 32768.0f;
                        } else if (bytes == 4) {
                            std::int32_t s32;
                            std::memcpy(&s32, p, 4);
                            v = static_cast<float>(s32) / 2147483648.0f;
                        }
                        mono += v;
                    }
                    mono *= 0.5f;
                }
                ring[head] = mono;
                head = (head + 1) % kFftSize;
            }
            capture.capture->ReleaseBuffer(frames);
        }
        if (broken) {  // the device changed (headphones plugged...): open it again
            capture.Release();
            continue;
        }
        // Analyze ~60 times a second.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFft >= std::chrono::milliseconds(16)) {
            const float dt = std::chrono::duration<float>(now - lastFft).count();
            lastFft = now;
            for (int i = 0; i < kFftSize; ++i) spectrum[i] = std::complex<float>(ring[(head + i) % kFftSize] * window[i], 0.0f);
            Fft(spectrum);
            const float rate = static_cast<float>(capture.format.rate);
            const float sensitivity = g_sensitivity.load();
            const float smoothing = std::clamp(g_smoothing.load(), 0.0f, 0.95f);
            float bass = 0.0f, total = 0.0f;
            Levels out;
            for (int b = 0; b < kBands; ++b) {
                // Bands on a musical scale: 40 Hz .. 16 kHz.
                const float lo = 40.0f * std::pow(400.0f, static_cast<float>(b) / kBands);
                const float hi = 40.0f * std::pow(400.0f, static_cast<float>(b + 1) / kBands);
                int i0 = static_cast<int>(lo / rate * kFftSize), i1 = static_cast<int>(hi / rate * kFftSize);
                i0 = std::clamp(i0, 1, kFftSize / 2 - 1);
                i1 = std::clamp(std::max(i1, i0 + 1), i0 + 1, kFftSize / 2);
                float sum = 0.0f;
                for (int i = i0; i < i1; ++i) sum = std::max(sum, std::abs(spectrum[i]));
                const float mag = sum / (kFftSize * 0.25f);
                // Decibels, plus a lift for the highs (they carry less energy).
                float db = 20.0f * std::log10(mag + 1e-7f) + 4.5f * std::log2(std::max(1.0f, (lo + hi) * 0.5f / 120.0f));
                float v = std::clamp((db + 62.0f) / 54.0f * sensitivity, 0.0f, 1.0f);
                v = std::pow(v, 1.15f);  // a little more contrast: quiet stays low, hits stand out
                // Up fast, down slowly (like a real meter).
                const float up = 1.0f - std::pow(0.0005f, dt);
                const float down = (1.0f - smoothing) * 6.0f * dt;
                smooth[b] = v > smooth[b] ? smooth[b] + (v - smooth[b]) * up : std::max(v, smooth[b] - down);
                out.bands[b] = smooth[b];
                // Quick: up at once, down fast (every hit shows).
                fast[b] = v > fast[b] ? fast[b] + (v - fast[b]) * 0.6f : std::max(v, fast[b] - 1.8f * dt);
                out.fast[b] = fast[b];
                // Peaks: hold the top for 0.35 s, then fall.
                if (v >= peaks[b]) {
                    peaks[b] = v;
                    peakAge[b] = 0.0f;
                } else {
                    peakAge[b] += dt;
                    if (peakAge[b] > 0.35f) peaks[b] = std::max(v, peaks[b] - 0.7f * dt);
                }
                out.peaks[b] = peaks[b];
                if (b < 8) bass += v;
                total += v;
            }
            bass /= 8.0f;
            total /= kBands;
            // A HIT: the bass jumps well above its recent average.
            bassAverage += (bass - bassAverage) * std::min(1.0f, dt * 3.0f);
            const float jump = std::max(0.0f, bass - lastBass) + std::max(0.0f, bass - bassAverage * 1.25f) * 0.5f;
            lastBass = bass;
            kick = std::max(kick * std::exp(-dt * 6.0f), std::clamp((jump - 0.04f) * 2.2f * sensitivity, 0.0f, 1.0f));
            out.kick = kick;
            // The wave: the newest ~1024 samples, 192 points, scaled to fill -1..1.
            float maxAbs = 0.0f;
            for (int i = 0; i < kWavePoints; ++i) {
                const std::size_t start = (head + kFftSize - 1024 + static_cast<std::size_t>(i) * 1024 / kWavePoints) % kFftSize;
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += ring[(start + static_cast<std::size_t>(k)) % kFftSize];
                out.wave[i] = sum * 0.25f;
                maxAbs = std::max(maxAbs, std::fabs(out.wave[i]));
            }
            waveScale = std::max(0.02f, waveScale + (maxAbs - waveScale) * std::min(1.0f, dt * (maxAbs > waveScale ? 10.0f : 1.5f)));
            for (float& w : out.wave) w = std::clamp(w / waveScale, -1.0f, 1.0f);
            bassSmooth = bass > bassSmooth ? bass : bassSmooth + (bass - bassSmooth) * std::min(1.0f, dt * 6.0f);
            levelSmooth += (total - levelSmooth) * std::min(1.0f, dt * 8.0f);
            out.bass = bassSmooth;
            out.level = levelSmooth;
            out.live = true;
            out.processOnly = capture.processOnly;
            std::lock_guard lock(g_mutex);
            g_levels = out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    capture.Release();
    CoUninitialize();
}

}  // namespace

void Want(bool want) {
    g_wanted.store(want);
    if (want) {
        std::lock_guard lock(g_threadMutex);
        if (!g_thread.joinable() && !g_stop.load()) g_thread = std::thread(ThreadMain);
    }
}

void SetTuning(float sensitivity, float smoothing) {
    g_sensitivity.store(sensitivity);
    g_smoothing.store(smoothing);
}

Levels Get() {
    std::lock_guard lock(g_mutex);
    return g_levels;
}

void Shutdown() {
    g_stop.store(true);
    std::lock_guard lock(g_threadMutex);
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace luma::audioviz
