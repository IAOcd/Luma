// =============================================================================
//  media.cpp - What's playing on Windows, and its controls (see media.h)
// =============================================================================
//  C++/WinRT: the Windows SDK's headers (winrt/...) turn Windows "WinRT" APIs
//  into normal C++ classes. Their async calls return an "operation"; we wait
//  for it with a time limit (Await) instead of blocking forever.
// =============================================================================
#include "core/media.h"

#include <windows.h>
#include <unknwn.h>
#include <wincodec.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "core/logger.h"
#include "perf/timer.h"

namespace luma::media {

namespace {

namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
using Microsoft::WRL::ComPtr;

std::atomic<std::uint32_t> g_wanted{0};
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::mutex g_threadMutex;  // starting / stopping the thread

// What the thread found (protected by g_mutex).
std::mutex g_mutex;
Track g_track;
std::int64_t g_positionAt = 0;  // when g_track.position was measured (perf ticks)
std::vector<std::uint8_t> g_art;

// Pending commands, in order (a few at most: clicks are slow compared to
// the thread). Protected by g_commandMutex.
enum class Command { None, PlayPause, Next, Previous, Seek, Shuffle, Repeat, Volume };
struct Pending {
    Command command = Command::None;
    double seekTo = 0.0;   // seconds (Seek), 0/1 (Shuffle), mode (Repeat), 0..1 (Volume)
};
std::mutex g_commandMutex;

// A better art offered from another thread (HD covers), waiting to be decoded here.
std::mutex g_offerMutex;
std::uint32_t g_offerTrack = 0;
std::vector<std::uint8_t> g_offerBytes;
float g_appVolume = -1.0f;  // the playing app's volume, as last read or set (media thread only)
Pending g_queue[8];
int g_queued = 0;

void Push(Command command, double seekTo = 0.0) {
    std::lock_guard lock(g_commandMutex);
    if (g_queued < 8) {
        g_queue[g_queued++] = Pending{command, seekTo};
    }
}

// Waits for a WinRT operation for at most "ms". False if it didn't finish.
template <typename Op>
bool Await(const Op& op, int ms) {
    if (op.wait_for(std::chrono::milliseconds(ms)) != winrt::Windows::Foundation::AsyncStatus::Completed) {
        try {
            op.Cancel();
        } catch (...) {
        }
        return false;
    }
    return true;
}

void CopyText(char* out, std::size_t size, const winrt::hstring& text) {
    const std::wstring_view w = text;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out,
                                      static_cast<int>(size) - 1, nullptr, nullptr);
    out[std::max(0, n)] = '\0';
}

// "Spotify.exe" / "Chrome" / "308046B0AF4A39CB" -> a name people recognize.
void AppName(char* out, std::size_t size, const winrt::hstring& id) {
    std::wstring s(id);
    std::wstring lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    struct Known {
        const wchar_t* part;
        const char* name;
    };
    static const Known kKnown[] = {
        {L"spotif", "Spotify"},{L"chrome", "Chrome"},       {L"msedge", "Edge"},
        {L"firefox", "Firefox"},       {L"opera", "Opera"},         {L"brave", "Brave"},
        {L"applemusic", "Apple Music"}, {L"itunes", "iTunes"},       {L"zunemusic", "Media Player"},
        {L"hkamori.liner", "Liner"}, {L"vlc", "VLC"},             {L"deezer", "Deezer"},       {L"tidal", "TIDAL"},
        {L"amazonmusic", "Amazon Music"}, {L"youtube", "YouTube Music"}, {L"soundcloud", "SoundCloud"},
    };
    for (const Known& k : kKnown) {
        if (lower.find(k.part) != std::wstring::npos) {
            strncpy_s(out, size, k.name, _TRUNCATE);
            return;
        }
    }
    CopyText(out, size, id);
    char* dot = std::strrchr(out, '.');
    if (dot != nullptr && _stricmp(dot, ".exe") == 0) {
        *dot = '\0';
    }
}

// Album art bytes (PNG/JPEG) -> 256x256 RGBA, cropped to a centered square.
// Also its main color (average of the colorful pixels).
UINT g_lastArtW = 0, g_lastArtH = 0;  // the original size of the last art (for the log)

// The album's palette: its pixels are sorted by hue into 12 "slices" of the
// color wheel (colorful, not too dark pixels only, weighted by how colorful
// and bright they are). The 5 heaviest slices give the 5 colors (each one the
// average of its pixels). A gray cover gets its accent, lighter and darker.
void Palette(const std::vector<std::uint8_t>& rgba, const float accent[3], float out[5][3], int& count) {
    double sum[12][3]{}, weight[12]{};
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 16) {
        const double r = rgba[i] / 255.0, g = rgba[i + 1] / 255.0, b = rgba[i + 2] / 255.0;
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
        const double sat = mx > 0.0 ? (mx - mn) / mx : 0.0;
        if (sat < 0.22 || mx < 0.18) continue;
        double h = 0.0;
        if (mx == r) h = std::fmod((g - b) / (mx - mn), 6.0);
        else if (mx == g) h = (b - r) / (mx - mn) + 2.0;
        else h = (r - g) / (mx - mn) + 4.0;
        if (h < 0.0) h += 6.0;
        const int slice = std::clamp(static_cast<int>(h * 2.0), 0, 11);
        const double w = sat * mx;
        sum[slice][0] += r * w;
        sum[slice][1] += g * w;
        sum[slice][2] += b * w;
        weight[slice] += w;
    }
    count = 0;
    bool used[12]{};
    for (int pick = 0; pick < 5; ++pick) {
        int best = -1;
        for (int k = 0; k < 12; ++k) {
            if (!used[k] && weight[k] > 0.0 && (best < 0 || weight[k] > weight[best])) best = k;
        }
        if (best < 0 || (count > 0 && weight[best] < weight[0] * 0.04)) break;  // too little of it
        used[best] = true;
        for (int c = 0; c < 3; ++c) out[count][c] = static_cast<float>(sum[best][c] / weight[best]);
        ++count;
    }
    if (count < 2) {
        // Few colors: the accent, plus a lighter and a darker one.
        count = 0;
        for (int c = 0; c < 3; ++c) {
            out[0][c] = accent[c];
            out[1][c] = accent[c] + (1.0f - accent[c]) * 0.55f;
            out[2][c] = accent[c] * 0.55f;
        }
        count = 3;
    }
}

bool DecodeArt(const std::vector<std::uint8_t>& bytes, std::vector<std::uint8_t>& rgba, float accent[3]) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
        return false;
    }
    g_lastArtW = w;
    g_lastArtH = h;
    const UINT side = std::min(w, h);
    const WICRect square{static_cast<INT>((w - side) / 2), static_cast<INT>((h - side) / 2), static_cast<INT>(side),
                         static_cast<INT>(side)};
    if (FAILED(factory->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(frame.Get(), &square)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(clipper.Get(), kArtSize, kArtSize, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }
    rgba.resize(static_cast<std::size_t>(kArtSize) * kArtSize * 4);
    if (FAILED(converter->CopyPixels(nullptr, kArtSize * 4, static_cast<UINT>(rgba.size()), rgba.data()))) {
        return false;
    }
    // Main color: colorful pixels count more than gray ones.
    double sum[3]{}, weight = 0.0;
    for (std::size_t i = 0; i < rgba.size(); i += 16) {  // every 4th pixel is plenty
        const double r = rgba[i] / 255.0, g = rgba[i + 1] / 255.0, b = rgba[i + 2] / 255.0;
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
        const double wgt = 0.05 + (mx - mn) * mx;  // saturation x brightness
        sum[0] += r * wgt;
        sum[1] += g * wgt;
        sum[2] += b * wgt;
        weight += wgt;
    }
    for (int k = 0; k < 3; ++k) {
        accent[k] = static_cast<float>(sum[k] / weight);
    }
    return true;
}

bool ReadThumbnail(const wss::IRandomAccessStreamReference& ref, std::vector<std::uint8_t>& out) {
    if (!ref) {
        return false;
    }
    auto open = ref.OpenReadAsync();
    if (!Await(open, 2000)) {
        return false;
    }
    const auto stream = open.GetResults();
    const auto size = static_cast<std::uint32_t>(std::min<std::uint64_t>(stream.Size(), 16u << 20));
    if (size == 0) {
        return false;
    }
    wss::DataReader reader(stream);
    auto load = reader.LoadAsync(size);
    if (!Await(load, 2000)) {
        return false;
    }
    out.resize(load.GetResults());
    reader.ReadBytes(out);
    return !out.empty();
}

// --- The app's volume (Windows' volume mixer) -------------------------------------------
// The media session only gives an app id ("Spotify.exe", "com.hkamori.liner"). We look for
// an audio session whose process or identifier contains its name.
std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::vector<std::wstring> NameTokens(const std::wstring& appId) {
    std::vector<std::wstring> tokens;
    std::wstring id = Lower(appId);
    const std::size_t bang = id.find(L'!');
    if (bang != std::wstring::npos) id = id.substr(0, bang);
    if (id.size() > 4 && id.compare(id.size() - 4, 4, L".exe") == 0) id.resize(id.size() - 4);
    const std::size_t under = id.find(L'_');
    if (under != std::wstring::npos) id = id.substr(0, under);
    if (id.size() >= 3) tokens.push_back(id);
    const std::size_t dot = id.rfind(L'.');
    if (dot != std::wstring::npos && id.size() - dot - 1 >= 3) tokens.push_back(id.substr(dot + 1));
    return tokens;
}

// The app's volume object (nullptr if not found). The caller releases it.
ISimpleAudioVolume* FindAppVolume(const std::wstring& appId) {
    const std::vector<std::wstring> tokens = NameTokens(appId);
    if (tokens.empty()) return nullptr;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) ||
        FAILED(manager->GetSessionEnumerator(&sessions))) {
        return nullptr;
    }
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2))) continue;
        std::wstring haystack;
        LPWSTR identifier = nullptr;
        if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&identifier)) && identifier != nullptr) {
            haystack = Lower(identifier);
            CoTaskMemFree(identifier);
        }
        DWORD pid = 0;
        if (SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0) {
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, path, &size)) haystack += L"|" + Lower(path);
                CloseHandle(process);
            }
        }
        for (const std::wstring& token : tokens) {
            if (haystack.find(token) != std::wstring::npos) {
                ISimpleAudioVolume* volume = nullptr;
                if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume)))) {
                    return volume;
                }
            }
        }
    }
    return nullptr;
}

void RunCommands(const wmc::GlobalSystemMediaTransportControlsSession& session) {
    Pending pending[8];
    int count = 0;
    {
        std::lock_guard lock(g_commandMutex);
        count = g_queued;
        for (int i = 0; i < count; ++i) {
            pending[i] = g_queue[i];
        }
        g_queued = 0;
    }
    if (!session) {
        return;
    }
    for (int i = 0; i < count; ++i) {
    try {
        switch (pending[i].command) {
            case Command::PlayPause: Await(session.TryTogglePlayPauseAsync(), 1500); break;
            case Command::Next: Await(session.TrySkipNextAsync(), 1500); break;
            case Command::Previous: Await(session.TrySkipPreviousAsync(), 1500); break;
            case Command::Seek: {
                // Position is in 100-nanosecond units.
                const auto ticks = static_cast<std::int64_t>(pending[i].seekTo * 10'000'000.0);
                Await(session.TryChangePlaybackPositionAsync(ticks), 1500);
                break;
            }
            case Command::Shuffle: Await(session.TryChangeShuffleActiveAsync(pending[i].seekTo != 0.0), 1500); break;
            case Command::Repeat: {
                const int mode = static_cast<int>(pending[i].seekTo);
                using Mode = winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
                Await(session.TryChangeAutoRepeatModeAsync(mode == 1 ? Mode::Track : mode == 2 ? Mode::List : Mode::None), 1500);
                break;
            }
            case Command::Volume: {
                if (ISimpleAudioVolume* volume = FindAppVolume(std::wstring(session.SourceAppUserModelId()))) {
                    volume->SetMasterVolume(static_cast<float>(std::clamp(pending[i].seekTo, 0.0, 1.0)), nullptr);
                    g_appVolume = static_cast<float>(std::clamp(pending[i].seekTo, 0.0, 1.0));
                    volume->Release();
                }
                break;
            }
            default: break;
        }
    } catch (...) {
    }
    }
}

void ThreadMain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wmc::GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    std::uint32_t trackCounter = 0, artCounter = 0;
    std::wstring lastKey;
    std::int64_t nextProps = 0;
    std::int64_t nextVolume = 0;
    // What only changes with the song (read once a second, kept in between).
    Track song;
    while (!g_stop.load()) {
        if (g_wanted.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        try {
            if (!manager) {
                auto request = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
                if (!Await(request, 3000)) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
                manager = request.GetResults();
                log::Info("Media: connected to Windows media sessions.");
            }
            const auto session = manager.GetCurrentSession();
            RunCommands(session);
            Track t;
            std::vector<std::uint8_t> art;
            bool newArt = false;
            if (!session) {
                song = Track{};
            }
            if (session) {
                t.present = true;
                AppName(t.app, sizeof(t.app), session.SourceAppUserModelId());
                const auto info = session.GetPlaybackInfo();
                t.playing = info.PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                const auto controls = info.Controls();
                t.canPlayPause = controls.IsPlayPauseToggleEnabled() || controls.IsPlayEnabled() || controls.IsPauseEnabled();
                t.canNext = controls.IsNextEnabled();
                t.canPrevious = controls.IsPreviousEnabled();
                t.canSeek = controls.IsPlaybackPositionEnabled();
                t.canShuffle = controls.IsShuffleEnabled();
                t.canRepeat = controls.IsRepeatEnabled();
                if (const auto shuffle = info.IsShuffleActive()) t.shuffle = shuffle.Value();
                if (const auto repeat = info.AutoRepeatMode()) {
                    using Mode = winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
                    t.repeat = repeat.Value() == Mode::Track ? 1 : repeat.Value() == Mode::List ? 2 : 0;
                }
                // The app's volume: looked up once a second (finding it walks the audio sessions).
                if (perf::Now() >= nextVolume) {
                    nextVolume = perf::Now() + static_cast<std::int64_t>(perf::g_ticksPerSecond);
                    g_appVolume = -1.0f;
                    if (ISimpleAudioVolume* volume = FindAppVolume(std::wstring(session.SourceAppUserModelId()))) {
                        float v = -1.0f;
                        if (SUCCEEDED(volume->GetMasterVolume(&v))) g_appVolume = v;
                        volume->Release();
                    }
                }
                t.volume = g_appVolume;
                const auto timeline = session.GetTimelineProperties();
                using seconds = std::chrono::duration<double>;
                t.duration = std::chrono::duration_cast<seconds>(timeline.EndTime() - timeline.StartTime()).count();
                double position = std::chrono::duration_cast<seconds>(timeline.Position()).count();
                // The position is "as of LastUpdatedTime": add what played since.
                if (t.playing) {
                    const auto since = winrt::clock::now() - timeline.LastUpdatedTime();
                    position += std::max(0.0, std::chrono::duration_cast<seconds>(since).count());
                }
                t.position = t.duration > 0.0 ? std::clamp(position, 0.0, t.duration) : std::max(0.0, position);

                // Title, artist, album and art: once a second is plenty.
                const std::int64_t now = perf::Now();
                if (now >= nextProps) {
                    nextProps = now + static_cast<std::int64_t>(perf::g_ticksPerSecond);
                    auto propsOp = session.TryGetMediaPropertiesAsync();
                    if (Await(propsOp, 1500)) {
                        const auto props = propsOp.GetResults();
                        CopyText(song.title, sizeof(song.title), props.Title());
                        CopyText(song.artist, sizeof(song.artist), props.Artist());
                        CopyText(song.album, sizeof(song.album), props.AlbumTitle());
                        const std::wstring key = std::wstring(props.Title()) + L"\n" + std::wstring(props.Artist()) +
                                                 L"\n" + std::wstring(props.AlbumTitle());
                        if (key != lastKey) {
                            lastKey = key;
                            ++trackCounter;
                            song.trackId = trackCounter;
                            song.artVersion = 0;  // until (and unless) the new art arrives
                            const float ivory[3] = {0.91f, 0.90f, 0.87f};
                            std::memcpy(song.accent, ivory, sizeof(ivory));
                            std::vector<std::uint8_t> bytes;
                            float accent[3];
                            song.artSourceSize = 0;
                            song.paletteCount = 2;
                            if (ReadThumbnail(props.Thumbnail(), bytes) && DecodeArt(bytes, art, accent)) {
                                newArt = true;
                                song.artVersion = ++artCounter;
                                song.artSourceSize = static_cast<int>(g_lastArtW);
                                std::memcpy(song.accent, accent, sizeof(accent));
                                Palette(art, accent, song.palette, song.paletteCount);
                            }
                            song.artChecked = true;
                            if (newArt) {
                                log::Info("Media: now playing \"{}\" - {} ({}), album art {}x{}.", song.title, song.artist,
                                          t.app, g_lastArtW, g_lastArtH);
                            } else {
                                log::Info("Media: now playing \"{}\" - {} ({}).", song.title, song.artist, t.app);
                            }
                        }
                    }
                }
            } else {
                lastKey.clear();
            }
            // An HD art for this song, found online? (decoded here, on this thread)
            {
                std::vector<std::uint8_t> offered;
                {
                    std::lock_guard lock(g_offerMutex);
                    if (g_offerTrack != 0 && g_offerTrack == song.trackId) {
                        offered = std::move(g_offerBytes);
                        g_offerTrack = 0;
                    }
                }
                float accent[3];
                if (!offered.empty() && DecodeArt(offered, art, accent)) {
                    newArt = true;
                    song.artVersion = ++artCounter;
                    song.artSourceSize = static_cast<int>(g_lastArtW);
                    std::memcpy(song.accent, accent, sizeof(accent));
                    Palette(art, accent, song.palette, song.paletteCount);
                    log::Info("Media: HD album art {}x{} for \"{}\".", g_lastArtW, g_lastArtH, song.title);
                }
            }
            std::memcpy(t.title, song.title, sizeof(t.title));
            std::memcpy(t.artist, song.artist, sizeof(t.artist));
            std::memcpy(t.album, song.album, sizeof(t.album));
            std::memcpy(t.accent, song.accent, sizeof(t.accent));
            std::memcpy(t.palette, song.palette, sizeof(t.palette));
            t.paletteCount = song.paletteCount;
            t.trackId = song.trackId;
            t.artVersion = song.artVersion;
            t.artSourceSize = song.artSourceSize;
            t.artChecked = song.artChecked;
            {
                std::lock_guard lock(g_mutex);
                if (newArt) {
                    g_art = std::move(art);
                }
                g_track = t;
                g_positionAt = perf::Now();
            }
        } catch (...) {
            // Windows said no (an app closed mid-call, etc.): try again later.
            manager = nullptr;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    manager = nullptr;
    winrt::uninit_apartment();
}

void EnsureThread() {
    std::lock_guard lock(g_threadMutex);
    if (!g_thread.joinable() && !g_stop.load()) {
        g_thread = std::thread(ThreadMain);
    }
}

}  // namespace

void Want(Source source, bool want) {
    if (want) {
        g_wanted.fetch_or(source);
        EnsureThread();
    } else {
        g_wanted.fetch_and(~static_cast<std::uint32_t>(source));
    }
}

void Shutdown() {
    g_stop.store(true);
    std::lock_guard lock(g_threadMutex);
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

Track Current() {
    std::lock_guard lock(g_mutex);
    Track t = g_track;
    if (t.playing) {
        t.position += perf::TicksToMs(perf::Now() - g_positionAt) / 1000.0;
        if (t.duration > 0.0) {
            t.position = std::min(t.position, t.duration);
        }
    }
    return t;
}

bool TakeArt(std::uint32_t knownVersion, std::vector<std::uint8_t>& out, std::uint32_t& version) {
    std::lock_guard lock(g_mutex);
    if (g_track.artVersion == 0 || g_track.artVersion == knownVersion || g_art.empty()) {
        return false;
    }
    out = g_art;
    version = g_track.artVersion;
    return true;
}

void PlayPause() {
    Push(Command::PlayPause);
}

void Next() {
    Push(Command::Next);
}

void Previous() {
    Push(Command::Previous);
}

void Seek(double seconds) {
    Push(Command::Seek, std::max(0.0, seconds));
}

void OfferArt(std::uint32_t trackId, std::vector<std::uint8_t> bytes) {
    std::lock_guard lock(g_offerMutex);
    g_offerTrack = trackId;
    g_offerBytes = std::move(bytes);
}

void SetShuffle(bool on) {
    Push(Command::Shuffle, on ? 1.0 : 0.0);
}

void SetRepeat(int mode) {
    Push(Command::Repeat, static_cast<double>(std::clamp(mode, 0, 2)));
}

void SetVolume(float volume) {
    {
        // Show the new volume right away (the thread confirms it a moment later).
        std::lock_guard lock(g_mutex);
        if (g_track.volume >= 0.0f) g_track.volume = std::clamp(volume, 0.0f, 1.0f);
    }
    // Only the newest volume matters: replace a pending one instead of queueing many.
    std::lock_guard lock(g_commandMutex);
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].command == Command::Volume) {
            g_queue[i].seekTo = volume;
            return;
        }
    }
    if (g_queued < 8) g_queue[g_queued++] = Pending{Command::Volume, volume};
}

}  // namespace luma::media
