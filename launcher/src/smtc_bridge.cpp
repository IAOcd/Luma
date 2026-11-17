// =============================================================================
//  smtc_bridge.cpp - Telling Windows what Luma's own player plays (see .h)
// =============================================================================
//  Windows' media controls ("SMTC") for a normal desktop window come through
//  ISystemMediaTransportControlsInterop::GetForWindow. All of it runs on its
//  own thread (Windows' async calls can't wait on the window's thread).
// =============================================================================
#include "smtc_bridge.h"

#include <unknwn.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "core/spotify.h"

namespace luma::launcher::smtc {

namespace {

using namespace winrt::Windows::Media;
using namespace std::chrono;

HWND g_window = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_stop{false};
std::thread g_thread;

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void Run() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    SystemMediaTransportControls smtc{nullptr};
    try {
        auto interop = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        winrt::check_hresult(interop->GetForWindow(g_window, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(smtc)));
    } catch (...) {
        return;  // (no media controls: Luma just won't show there)
    }
    smtc.IsPlayEnabled(true);
    smtc.IsPauseEnabled(true);
    smtc.IsNextEnabled(true);
    smtc.IsPreviousEnabled(true);
    smtc.IsEnabled(false);
    // the buttons (media keys, Windows' panel, Luma's own Home): to Spotify
    smtc.ButtonPressed([](const SystemMediaTransportControls&, const SystemMediaTransportControlsButtonPressedEventArgs& e) {
        switch (e.Button()) {
            case SystemMediaTransportControlsButton::Play: spotify::PlayerPlay(true); break;
            case SystemMediaTransportControlsButton::Pause: spotify::PlayerPlay(false); break;
            case SystemMediaTransportControlsButton::Next: spotify::PlayerNext(); break;
            case SystemMediaTransportControlsButton::Previous: spotify::PlayerPrevious(); break;
            default: break;
        }
    });
    smtc.PlaybackPositionChangeRequested([](const SystemMediaTransportControls&, const PlaybackPositionChangeRequestedEventArgs& e) {
        spotify::PlayerSeek(duration_cast<duration<double>>(e.RequestedPlaybackPosition()).count());
    });
    std::string shownTrack;
    std::wstring shownArt;
    bool enabled = false;
    int lastStatus = -1;
    auto lastTimeline = steady_clock::now() - seconds(10);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(milliseconds(150));
        try {
            const bool active = g_active.load();
            if (!active) {
                if (enabled) {
                    smtc.IsEnabled(false);
                    enabled = false;
                    shownTrack.clear();
                }
                continue;
            }
            const spotify::Player pl = spotify::GetPlayer();
            if (pl.title.empty()) continue;
            if (!enabled) {
                smtc.IsEnabled(true);
                enabled = true;
            }
            // the song (once per song, and again when its cover arrives)
            if (pl.trackId != shownTrack || pl.artPath != shownArt) {
                shownTrack = pl.trackId;
                shownArt = pl.artPath;
                auto du = smtc.DisplayUpdater();
                du.Type(MediaPlaybackType::Music);
                du.MusicProperties().Title(Wide(pl.title));
                du.MusicProperties().Artist(Wide(pl.artist));
                du.MusicProperties().AlbumTitle(Wide(pl.album));
                if (!pl.artPath.empty()) {
                    try {
                        auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(pl.artPath).get();
                        du.Thumbnail(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(file));
                    } catch (...) {
                    }
                }
                du.Update();
                lastTimeline = steady_clock::now() - seconds(10);
            }
            const int status = pl.playing ? 1 : 2;
            if (status != lastStatus) {
                lastStatus = status;
                smtc.PlaybackStatus(pl.playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
                lastTimeline = steady_clock::now() - seconds(10);
            }
            // where it is (every second: Windows moves it on by itself in between)
            if (steady_clock::now() - lastTimeline > seconds(1) && pl.durationMs > 0) {
                lastTimeline = steady_clock::now();
                SystemMediaTransportControlsTimelineProperties tl;
                tl.StartTime(winrt::Windows::Foundation::TimeSpan{0});
                tl.EndTime(duration_cast<winrt::Windows::Foundation::TimeSpan>(milliseconds(pl.durationMs)));
                tl.MinSeekTime(winrt::Windows::Foundation::TimeSpan{0});
                tl.MaxSeekTime(duration_cast<winrt::Windows::Foundation::TimeSpan>(milliseconds(pl.durationMs)));
                tl.Position(duration_cast<winrt::Windows::Foundation::TimeSpan>(
                    milliseconds(static_cast<long long>(spotify::PositionSeconds(pl) * 1000.0))));
                smtc.UpdateTimelineProperties(tl);
            }
        } catch (...) {
        }
    }
    try {
        smtc.IsEnabled(false);
    } catch (...) {
    }
}

}  // namespace

void Init(HWND window) {
    g_window = window;
    g_stop.store(false);
    if (!g_thread.joinable()) g_thread = std::thread(Run);
}

void Update(bool active) {
    g_active.store(active);
}

void Shutdown() {
    g_stop.store(true);
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace luma::launcher::smtc
