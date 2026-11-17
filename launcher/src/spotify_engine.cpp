// =============================================================================
//  spotify_engine.cpp - Luma's own Spotify player (see spotify_engine.h)
// =============================================================================
#include "spotify_engine.h"

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/logger.h"
#include "core/paths.h"
#include "core/spotify.h"

namespace luma::launcher::spotifyengine {

namespace {

constexpr unsigned short kPort = 43822;

std::mutex g_mutex;
State g_state = State::Off;
std::string g_deviceId, g_error;
std::string g_key;  // the random key of this session (the page must say it)
std::thread g_server;
std::atomic<bool> g_stop{false};
HANDLE g_job = nullptr;      // Edge and its helpers live in this "job": closing it closes them all
HANDLE g_edge = nullptr;

void Set(State s, std::string error = {}) {
    std::lock_guard lock(g_mutex);
    g_state = s;
    if (!error.empty()) g_error = std::move(error);
}

std::string RandomKey() {
    unsigned char b[18];
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : b) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

// The player page. Spotify's SDK (from Spotify's own server) makes the
// "Luma" device; the pass comes from the launcher (this PC only).
std::string Page() {
    return R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Luma player</title></head>
<body style="background:#111;color:#999;font-family:Segoe UI,sans-serif">Luma
<script>
const K = new URLSearchParams(location.search).get('k');
const say = (what, text) => fetch('/' + what + '?k=' + K + '&v=' + encodeURIComponent(text || '')).catch(() => {});
async function token() {
  for (;;) {
    try {
      const r = await fetch('/token?k=' + K);
      if (r.ok) { const t = await r.text(); if (t) return t; }
    } catch (e) {}
    await new Promise(s => setTimeout(s, 700));
  }
}
window.onSpotifyWebPlaybackSDKReady = () => {
  const player = new Spotify.Player({ name: 'Luma', getOAuthToken: cb => token().then(cb), volume: 0.85 });
  player.addListener('ready', ({ device_id }) => say('ready', device_id));
  player.addListener('not_ready', () => say('log', 'not_ready'));
  player.addListener('initialization_error', ({ message }) => say('error', 'init: ' + message));
  player.addListener('authentication_error', ({ message }) => say('error', 'auth: ' + message));
  player.addListener('account_error', ({ message }) => say('error', 'premium: ' + message));
  player.addListener('playback_error', ({ message }) => say('log', 'playback: ' + message));
  // Tell Windows what plays (name, artists, album, cover, where it is): Luma's
  // Home, the lyrics and the game read it from there; Windows' media keys
  // and Luma's buttons control it through these handlers.
  const ms = navigator.mediaSession;
  player.addListener('player_state_changed', s => {
    if (!s || !s.track_window || !s.track_window.current_track) return;
    const t = s.track_window.current_track;
    const artists = (t.artists || []).map(a => a.name).join(', ');
    document.title = t.name + ' - ' + artists;
    if (!ms) return;
    ms.metadata = new MediaMetadata({
      title: t.name, artist: artists, album: (t.album && t.album.name) || '',
      artwork: ((t.album && t.album.images) || []).map(i => ({ src: i.url, sizes: (i.width || 300) + 'x' + (i.height || 300), type: 'image/jpeg' }))
    });
    ms.playbackState = s.paused ? 'paused' : 'playing';
    try { ms.setPositionState({ duration: s.duration / 1000, position: Math.min(s.position, s.duration) / 1000, playbackRate: 1 }); } catch (e) {}
  });
  if (ms) {
    ms.setActionHandler('play', () => player.resume());
    ms.setActionHandler('pause', () => player.pause());
    ms.setActionHandler('nexttrack', () => player.nextTrack());
    ms.setActionHandler('previoustrack', () => player.previousTrack());
    try { ms.setActionHandler('seekto', d => player.seek(Math.round(d.seekTime * 1000))); } catch (e) {}
  }
  player.connect().then(ok => say('log', ok ? 'connecting' : 'connect failed'));
  document.addEventListener('click', () => player.activateElement && player.activateElement());
};
</script>
<script src="https://sdk.scdn.co/spotify-player.js"></script>
</body></html>)HTML";
}

std::string QueryValue(const std::string& query, const char* key) {
    const std::string k = std::string(key) + "=";
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        if (query.compare(pos, k.size(), k) == 0) {
            std::string v = query.substr(pos + k.size(), end - pos - k.size());
            std::string out;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i] == '%' && i + 2 < v.size()) {
                    out += static_cast<char>(std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    out += v[i] == '+' ? ' ' : v[i];
                }
            }
            return out;
        }
        pos = end + 1;
    }
    return {};
}

// What happens in the player, written to %APPDATA%\Luma\logs\spotify_player.txt
// (the launcher keeps no other log): to see why it didn't play.
void Note(const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(paths::DataDir() / L"logs", ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (paths::DataDir() / L"logs" / L"spotify_player.txt").c_str(), L"a") == 0 && f != nullptr) {
        std::fprintf(f, "%s%s\n", stamp, text.c_str());
        std::fclose(f);
    }
}

void Reply(SOCKET c, const char* status, const char* type, const std::string& body) {
    char head[256];
    std::snprintf(head, sizeof(head),
                  "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, type,
                  body.size());
    std::string all = head + body;
    send(c, all.data(), static_cast<int>(all.size()), 0);
}

void Handle(SOCKET c) {
    char buffer[4096];
    const int n = recv(c, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;
    const std::string req(buffer, static_cast<std::size_t>(n));
    // "GET /path?query HTTP/1.1"
    if (req.rfind("GET ", 0) != 0) {
        Reply(c, "405 Method Not Allowed", "text/plain", "");
        return;
    }
    const std::size_t end = req.find(' ', 4);
    const std::string target = req.substr(4, end == std::string::npos ? std::string::npos : end - 4);
    const std::size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    const std::string query = q == std::string::npos ? std::string() : target.substr(q + 1);
    std::string key;
    {
        std::lock_guard lock(g_mutex);
        key = g_key;
    }
    if (QueryValue(query, "k") != key) {  // (only the page Luma opened knows the key)
        Reply(c, "403 Forbidden", "text/plain", "");
        return;
    }
    if (path == "/") {
        Reply(c, "200 OK", "text/html; charset=utf-8", Page());
    } else if (path == "/token") {
        const std::string token = spotify::SharedToken();
        if (token.empty()) Note("token asked: not ready yet");
        Reply(c, token.empty() ? "503 Service Unavailable" : "200 OK", "text/plain", token);
    } else if (path == "/ready") {
        {
            std::lock_guard lock(g_mutex);
            g_deviceId = QueryValue(query, "v");
            g_state = State::Ready;
        }
        log::Info("Luma's Spotify player is ready (the \"Luma\" device).");
        Note("ready: the Luma device exists");
        spotify::RequestDevices();
        Reply(c, "204 No Content", "text/plain", "");
    } else if (path == "/error") {
        const std::string what = QueryValue(query, "v");
        log::Warn("Luma's Spotify player: {}", what);
        Note("error: " + what);
        Set(State::Error, what);
        Reply(c, "204 No Content", "text/plain", "");
    } else if (path == "/log") {
        log::Info("Luma's Spotify player: {}", QueryValue(query, "v"));
        Note(QueryValue(query, "v"));
        Reply(c, "204 No Content", "text/plain", "");
    } else {
        Reply(c, "404 Not Found", "text/plain", "");
    }
}

// Edge's windows, invisible: no window on the screen and no icon in the
// taskbar (it stays inside Luma: closed when Luma closes).
BOOL CALLBACK HideOne(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return TRUE;
    BOOL inJob = FALSE;
    if (g_job != nullptr && IsProcessInJob(process, g_job, &inJob) && inJob && IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (ex | WS_EX_TOOLWINDOW) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW));
    }
    CloseHandle(process);
    return TRUE;
}

void Serve(SOCKET listener) {
    ULONGLONG lastHide = 0;
    while (!g_stop.load()) {
        if (GetTickCount64() - lastHide > 250) {
            lastHide = GetTickCount64();
            EnumWindows(HideOne, 0);
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval wait{0, 200000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        try {
            Handle(c);
        } catch (...) {
        }
        closesocket(c);
    }
    closesocket(listener);
    WSACleanup();
}

// Where Microsoft Edge is (Windows says it in "App Paths"; else the usual folders).
std::wstring EdgePath() {
    wchar_t path[MAX_PATH * 2];
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", nullptr, RRF_RT_REG_SZ,
                     nullptr, path, &size) == ERROR_SUCCESS) {
        return path;
    }
    for (const wchar_t* p : {L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
                             L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe"}) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

}  // namespace

void Start() {
    {
        std::lock_guard lock(g_mutex);
        if (g_state == State::Starting || g_state == State::Ready) return;
        g_error.clear();
        g_deviceId.clear();
        g_key = RandomKey();
    }
    Stop();
    const std::wstring edge = EdgePath();
    if (edge.empty()) {
        Set(State::NoEdge);
        return;
    }
    // the tiny server (127.0.0.1 only)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kPort);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        Set(State::Error, "port 43822 is busy");
        return;
    }
    g_stop.store(false);
    g_server = std::thread(Serve, listener);
    // Edge: its own profile, hidden (out of the screen), sound allowed without a click
    std::string key;
    {
        std::lock_guard lock(g_mutex);
        key = g_key;
    }
    const std::filesystem::path profile = paths::DataDir() / L"edge";
    std::wstring cmd = L"\"" + edge + L"\" --app=\"http://127.0.0.1:43822/?k=" + std::wstring(key.begin(), key.end()) + L"\"" +
                       L" --user-data-dir=\"" + profile.wstring() + L"\"" +
                       L" --autoplay-policy=no-user-gesture-required --no-first-run --no-default-browser-check"
                       L" --window-position=-32000,-32000 --window-size=420,320 --disable-background-timer-throttling"
                       L" --disable-renderer-backgrounding --disable-backgrounding-occluded-windows";
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Set(State::Error, "Edge didn't open");
        return;
    }
    if (g_job != nullptr) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_edge = pi.hProcess;
    Set(State::Starting);
    log::Info("Luma's Spotify player: Edge opened (hidden).");
    Note("Edge opened (hidden)");
}

void Stop() {
    if (g_job != nullptr) {
        CloseHandle(g_job);  // (closes Edge and everything it opened)
        g_job = nullptr;
    }
    if (g_edge != nullptr) {
        TerminateProcess(g_edge, 0);
        CloseHandle(g_edge);
        g_edge = nullptr;
    }
    g_stop.store(true);
    if (g_server.joinable()) g_server.join();
    std::lock_guard lock(g_mutex);
    if (g_state != State::NoEdge) g_state = State::Off;
    g_deviceId.clear();
}

State GetState() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

std::string DeviceId() {
    std::lock_guard lock(g_mutex);
    return g_deviceId;
}

std::string Error() {
    std::lock_guard lock(g_mutex);
    return g_error;
}

}  // namespace luma::launcher::spotifyengine
