// =============================================================================
//  spotify.cpp - Optional Spotify connection (see spotify.h)
// =============================================================================
#include "core/spotify.h"

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <map>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/lyrics.h"
#include "core/media.h"
#include "core/paths.h"

namespace luma::spotify {

namespace {

constexpr unsigned short kPort = 43821;
// What Luma asks Spotify for (you accept it on Spotify's page). Seeing what
// plays and your queue; controlling the music (Premium); your library (Liked
// songs: read and add/remove), your playlists and what you played lately.
constexpr const char* kScope =
    "user-read-playback-state user-read-currently-playing user-modify-playback-state "
    "user-library-read user-library-modify playlist-read-private playlist-read-collaborative "
    "user-read-recently-played user-read-private user-top-read user-follow-read "
    "playlist-modify-public playlist-modify-private streaming user-read-email";  // (streaming: Luma's own player)  // (private: only to know if the account is Premium)
// The permission that the newest Luma needs (if the saved one lacks it: connect again).
constexpr const char* kNewestScope = "streaming";

std::mutex g_mutex;          // protects everything below that's not atomic
std::string g_clientId;
Status g_status;
std::vector<QueueItem> g_queue;
std::uint32_t g_queueVersion = 0;

std::atomic<bool> g_active{false};
std::atomic<bool> g_prefetch{true};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_connectRequested{false};
std::atomic<bool> g_disconnectRequested{false};
std::thread g_thread;
std::mutex g_threadMutex;

void SetState(State state, std::string error = {}) {
    std::lock_guard lock(g_mutex);
    g_status.state = state;
    g_status.error = std::move(error);
}

std::string ClientId() {
    std::lock_guard lock(g_mutex);
    return g_clientId;
}

// --- Small helpers -------------------------------------------------------------------------

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char hex[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i] == '+' ? ' ' : s[i]);
        }
    }
    return out;
}

std::string QueryParam(const std::string& query, const char* key) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(pos, end - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, key) == 0 && std::strlen(key) == eq) {
            return UrlDecode(pair.substr(eq + 1));
        }
        pos = end + 1;
    }
    return {};
}

std::string Base64Url(const std::uint8_t* data, DWORD size) {
    DWORD length = 0;
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
    std::string out(length, '\0');
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length);
    out.resize(length);
    for (char& c : out) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (!out.empty() && (out.back() == '=' || out.back() == '\0')) out.pop_back();
    return out;
}

// A random string (for PKCE's secret and the "state" check).
std::string RandomToken(DWORD bytes) {
    std::vector<std::uint8_t> buffer(bytes);
    BCryptGenRandom(nullptr, buffer.data(), bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return Base64Url(buffer.data(), bytes);
}

std::string Sha256Base64Url(const std::string& text) {
    std::uint8_t hash[32]{};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
               static_cast<ULONG>(text.size()), hash, sizeof(hash));
    return Base64Url(hash, sizeof(hash));
}

// One HTTPS request. Returns the status code (0 = no connection).
int Https(const wchar_t* host, const wchar_t* method, const std::string& path, const std::string& headers,
          const std::string& body, std::string& response) {
    response.clear();
    HINTERNET session = WinHttpOpen(L"Luma", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 6000, 6000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    const std::wstring wpath = Widen(path);
    HINTERNET request = connection != nullptr ? WinHttpOpenRequest(connection, method, wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                              : nullptr;
    int status = 0;
    if (request != nullptr) {
        const std::wstring wheaders = Widen(headers);
        if (WinHttpSendRequest(request, wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
                               wheaders.empty() ? 0 : static_cast<DWORD>(-1L),
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0;
            DWORD size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0 || response.size() > 2 * 1024 * 1024) break;
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                response.append(chunk.data(), read);
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status;
}

// --- The saved pass (refresh token), encrypted with DPAPI -----------------------------
std::filesystem::path PassPath() {
    return paths::DataDir() / L"spotify.dat";
}

void SavePass(const std::string& token) {
    DATA_BLOB in{static_cast<DWORD>(token.size()), reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Luma Spotify", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    std::error_code ec;
    std::filesystem::create_directories(PassPath().parent_path(), ec);
    std::ofstream(PassPath(), std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
}

std::string LoadPass() {
    std::ifstream file(PassPath(), std::ios::binary);
    if (!file) return {};
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DATA_BLOB in{static_cast<DWORD>(data.size()), reinterpret_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (data.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string token(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return token;
}

void DeletePass() {
    std::error_code ec;
    std::filesystem::remove(PassPath(), ec);
    std::filesystem::remove(paths::DataDir() / L"spotify_scope.txt", ec);
}

// What permissions the saved pass has (not secret: just their names).
void SaveScope(const std::string& scope) {
    std::ofstream(paths::DataDir() / L"spotify_scope.txt", std::ios::binary | std::ios::trunc) << scope;
}
std::string LoadScope() {
    std::ifstream file(paths::DataDir() / L"spotify_scope.txt", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
bool ScopeOutdated() {
    return std::filesystem::exists(PassPath()) && LoadScope().find(kNewestScope) == std::string::npos;
}

// --- Tokens ---------------------------------------------------------------------------------
std::string g_accessToken;  // only used by the thread
std::chrono::steady_clock::time_point g_accessExpires;

// Trades a form (a new code, or the saved pass) for an access token.
// Returns 1 = ok, 0 = rejected (the pass is no longer valid), -1 = no connection.
int Redeem(const std::string& form) {
    std::string body;
    const int status = Https(L"accounts.spotify.com", L"POST", "/api/token",
                             "Content-Type: application/x-www-form-urlencoded\r\n", form, body);
    if (status == 0 || status >= 500) return -1;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (status != 200 || !j.is_object() || !j.contains("access_token") || !j["access_token"].is_string()) {
        log::Warn("Spotify: the token was rejected ({}: {}).", status,
                  j.is_object() ? j.value("error", std::string("?")) : std::string("?"));
        return 0;
    }
    g_accessToken = j["access_token"].get<std::string>();
    g_accessExpires = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60, j.value("expires_in", 3600) - 60));
    // Spotify may give a new pass (they rotate): keep the newest.
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) SavePass(j["refresh_token"].get<std::string>());
    if (j.contains("scope") && j["scope"].is_string()) SaveScope(j["scope"].get<std::string>());
    return 1;
}

// A valid access token, refreshing it with the saved pass if needed.
bool EnsureToken() {
    if (!g_accessToken.empty() && std::chrono::steady_clock::now() < g_accessExpires) return true;
    const std::string pass = LoadPass();
    const std::string clientId = ClientId();
    if (pass.empty() || clientId.empty()) {
        SetState(clientId.empty() ? State::NoClientId : State::SignedOut);
        return false;
    }
    int r = Redeem("grant_type=refresh_token&refresh_token=" + UrlEncode(pass) + "&client_id=" + UrlEncode(clientId));
    if (r == 0) {
        // The game and the launcher share this pass, and Spotify may give a new
        // one each time it's used: if the other one just changed it, use that.
        const std::string newer = LoadPass();
        if (!newer.empty() && newer != pass) {
            r = Redeem("grant_type=refresh_token&refresh_token=" + UrlEncode(newer) + "&client_id=" + UrlEncode(clientId));
        }
    }
    if (r == 0) {
        DeletePass();  // revoked or expired: you need to connect again
        g_accessToken.clear();
        SetState(State::SignedOut);
        return false;
    }
    if (r < 0) return false;  // no internet right now: try later, keep the pass
    SetState(State::Connected);
    return true;
}

// --- Step 1: the browser and the tiny server on 127.0.0.1 ------------------------------
// Returns the query that arrived ("code=...&state=..."), or empty if cancelled.
std::string WaitForRedirect(SOCKET listener) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!g_stop.load() && !g_disconnectRequested.load() && std::chrono::steady_clock::now() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval wait{0, 250000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        char buffer[8192];
        const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        const std::string request(buffer, received > 0 ? static_cast<std::size_t>(received) : 0);
        // First line: "GET /callback?code=...&state=... HTTP/1.1"
        std::string query;
        const std::size_t q = request.find('?');
        const std::size_t end = request.find(' ', q == std::string::npos ? 0 : q);
        if (request.rfind("GET /callback?", 0) == 0 && q != std::string::npos && end != std::string::npos && q < end) {
            query = request.substr(q + 1, end - q - 1);
        }
        const bool useful = !QueryParam(query, "code").empty() || !QueryParam(query, "error").empty();
        const char* page =
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>Luma</title><style>"
            "body{background:#1B1D1F;color:#E9E6DF;font-family:Segoe UI,sans-serif;display:flex;"
            "align-items:center;justify-content:center;height:100vh;margin:0}"
            "div{text-align:center}h1{font-weight:600}p{opacity:.6}</style></head><body><div>"
            "<h1>Luma + Spotify</h1><p>You can close this tab and go back to the game.</p>"
            "<p>Ya puedes cerrar esta pesta&ntilde;a y volver al juego.</p></div></body></html>";
        char response[2048];
        std::snprintf(response, sizeof(response),
                      "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      useful ? "200 OK" : "404 Not Found", useful ? std::strlen(page) : 0, useful ? page : "");
        send(client, response, static_cast<int>(std::strlen(response)), 0);
        closesocket(client);
        if (useful) return query;
    }
    return {};
}

void RunConnect() {
    const std::string clientId = ClientId();
    if (clientId.empty()) {
        SetState(State::NoClientId);
        return;
    }
    SetState(State::SigningIn);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // only this PC can reach it
    address.sin_port = htons(kPort);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        SetState(State::Error, "port 43821 is busy");
        return;
    }
    const std::string verifier = RandomToken(48);
    const std::string state = RandomToken(16);
    const std::string url = "https://accounts.spotify.com/authorize?client_id=" + UrlEncode(clientId) +
                            "&response_type=code&redirect_uri=" + UrlEncode(kRedirectUri) + "&scope=" + UrlEncode(kScope) +
                            "&code_challenge_method=S256&code_challenge=" + Sha256Base64Url(verifier) + "&state=" + state;
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const std::string query = WaitForRedirect(listener);
    closesocket(listener);
    WSACleanup();
    if (query.empty()) {
        SetState(State::SignedOut);
        return;
    }
    if (QueryParam(query, "state") != state) {
        SetState(State::Error, "unexpected answer");  // not the answer to OUR request
        return;
    }
    const std::string code = QueryParam(query, "code");
    if (code.empty()) {
        const std::string error = QueryParam(query, "error");
        SetState(error == "access_denied" ? State::SignedOut : State::Error, error);
        return;
    }
    const int r = Redeem("grant_type=authorization_code&code=" + UrlEncode(code) + "&redirect_uri=" + UrlEncode(kRedirectUri) +
                         "&client_id=" + UrlEncode(clientId) + "&code_verifier=" + verifier);
    if (r <= 0) {
        SetState(State::Error, r < 0 ? "no connection" : "Spotify rejected the code (check the Client ID and Redirect URI)");
        return;
    }
    log::Info("Spotify: connected.");
    SetState(State::Connected);
}

// --- Your account (name, Premium or not) ----------------------------------------------------
bool g_profileAsked = false;  // (only the thread)

void FetchProfile() {
    if (!EnsureToken()) return;
    std::string body;
    const int status = Https(L"api.spotify.com", L"GET", "/v1/me", "Authorization: Bearer " + g_accessToken + "\r\n", {}, body);
    if (status == 401) {
        g_accessToken.clear();
        return;
    }
    g_profileAsked = true;
    if (status != 200) return;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard lock(g_mutex);
    g_status.user = j.value("display_name", j.value("id", std::string()));
    const std::string product = j.value("product", std::string());
    g_status.premium = product.empty() ? -1 : (product == "premium" ? 1 : 0);
}

// --- The queue ------------------------------------------------------------------------------
void PublishQueue(std::vector<QueueItem> items) {
    std::lock_guard lock(g_mutex);
    g_queue = std::move(items);
    ++g_queueVersion;
}

void FetchQueue() {
    if (!EnsureToken()) return;
    std::string body;
    const int status = Https(L"api.spotify.com", L"GET", "/v1/me/player/queue",
                             "Authorization: Bearer " + g_accessToken + "\r\n", {}, body);
    if (status == 401) {  // the pass ran out early: refresh next time
        g_accessToken.clear();
        return;
    }
    if (status != 200) {
        if (status == 429) log::Info("Spotify: asked too often, waiting.");
        return;
    }
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("queue") || !j["queue"].is_array()) return;
    std::vector<QueueItem> items;
    for (const auto& t : j["queue"]) {
        if (items.size() >= 10) break;
        if (!t.is_object() || t.value("type", std::string()) != "track") continue;  // skip podcasts
        QueueItem item;
        item.title = t.value("name", std::string());
        if (t.contains("artists") && t["artists"].is_array()) {
            for (const auto& a : t["artists"]) {
                if (!a.is_object()) continue;
                if (!item.artist.empty()) item.artist += ", ";
                item.artist += a.value("name", std::string());
            }
        }
        if (t.contains("album") && t["album"].is_object()) item.album = t["album"].value("name", std::string());
        item.seconds = t.value("duration_ms", 0) / 1000;
        if (!item.title.empty()) items.push_back(std::move(item));
    }
    // Look up the lyrics of the next 3 songs now (saved on disk for later).
    for (std::size_t i = 0; g_prefetch.load() && i < items.size() && i < 3; ++i) {
        lyrics::Prefetch(items[i].title, items[i].artist, items[i].album, items[i].seconds);
    }
    PublishQueue(std::move(items));
}

void EnsureThread();  // (below)

// --- THE PLAYER (Web API) ---------------------------------------------------------------------
// What plays in your Spotify (on any device), asked about once a second while
// someone wants it (the launcher's Spotify page or Home), and the commands
// (play, pause, next...). The commands go to a small list that this thread
// sends; the screen changes at once (it assumes they'll work) and the next
// answer from Spotify confirms it.

std::atomic<bool> g_playerWanted{false};
Player g_player;                      // (g_mutex)
std::vector<Device> g_devices;        // (g_mutex)
std::uint32_t g_devicesVersion = 0;   // (g_mutex)
std::string g_playerError;            // (g_mutex) "premium", "no_device", "" ...
std::chrono::steady_clock::time_point g_playerErrorAt;
std::atomic<bool> g_devicesWanted{false};
std::chrono::steady_clock::time_point g_nextPoll;

struct Command {
    enum Kind { Play, Pause, Next, Previous, Seek, Shuffle, Repeat, Volume, Transfer, Like, Raw, Mix } kind;
    long long value = 0;
    std::string text;  // a device id, a track id, a path, artists
    const char* method = "PUT";
    std::string body;
};
std::vector<Command> g_commands;  // (g_mutex)

// A call to Spotify's Web API with your pass. Gives the status (-1 = not
// connected or no internet) and the answer in "out".
int Api(const wchar_t* method, const std::string& path, const std::string& body, std::string& out) {
    out.clear();
    if (!EnsureToken()) return -1;
    std::string headers = "Authorization: Bearer " + g_accessToken + "\r\n";
    if (!body.empty()) headers += "Content-Type: application/json\r\n";
    int status = Https(L"api.spotify.com", method, path, headers, body, out);
    if (status == 401) {  // the pass ran out early: a new one, and once more
        g_accessToken.clear();
        if (!EnsureToken()) return -1;
        headers = "Authorization: Bearer " + g_accessToken + "\r\n";
        if (!body.empty()) headers += "Content-Type: application/json\r\n";
        status = Https(L"api.spotify.com", method, path, headers, body, out);
    }
    return status == 0 ? -1 : status;
}

void SetPlayerError(const std::string& error) {
    std::lock_guard lock(g_mutex);
    g_playerError = error;
    g_playerErrorAt = std::chrono::steady_clock::now();
}

// Spotify's reason for a refusal: "premium" (only Premium can control),
// "no_device" (nothing open to play on), or a short text.
std::string ReasonOf(int status, const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    std::string reason, message;
    if (j.is_object() && j.contains("error") && j["error"].is_object()) {
        reason = j["error"].value("reason", std::string());
        message = j["error"].value("message", std::string());
    }
    if (reason == "PREMIUM_REQUIRED" || (status == 403 && message.find("remium") != std::string::npos)) return "premium";
    if (reason == "NO_ACTIVE_DEVICE" || status == 404) return "no_device";
    if (status == 429) return "busy";
    return message.empty() ? "error " + std::to_string(status) : message;
}

std::string ArtistsOf(const nlohmann::json& item) {
    std::string out;
    const auto artists = item.find("artists");
    if (artists == item.end() || !artists->is_array()) return out;
    for (const auto& a : *artists) {
        if (!a.is_object()) continue;
        if (!out.empty()) out += ", ";
        out += a.value("name", std::string());
    }
    return out;
}

// The album art of a song, downloaded once to %APPDATA%\Luma\covers\spotify\.
std::wstring DownloadArt(const nlohmann::json& images) {
    if (!images.is_array() || images.empty()) return {};
    // the one closest to 300 px (Spotify gives 640, 300 and 64)
    const nlohmann::json* best = nullptr;
    for (const auto& im : images) {
        if (!im.is_object() || !im.contains("url") || !im["url"].is_string()) continue;
        if (best == nullptr || std::abs(im.value("width", 0) - 300) < std::abs(best->value("width", 0) - 300)) best = &im;
    }
    if (best == nullptr) return {};
    const std::string url = (*best)["url"].get<std::string>();
    const std::string prefix = "https://i.scdn.co";
    if (url.rfind(prefix, 0) != 0) return {};  // (only Spotify's own image server)
    const std::string path = url.substr(prefix.size());
    std::string name = path.substr(path.find_last_of('/') + 1);
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    const std::filesystem::path dir = paths::DataDir() / L"covers" / L"spotify";
    const std::filesystem::path file = dir / (Widen(name) + L".jpg");
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) return file.wstring();
    std::string data;
    if (Https(L"i.scdn.co", L"GET", path, {}, {}, data) != 200 || data.size() < 100) return {};
    std::filesystem::create_directories(dir, ec);
    std::ofstream(file, std::ios::binary | std::ios::trunc).write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.wstring();
}

void FetchLiked(const std::string& trackId) {
    if (trackId.empty()) return;
    std::string body;
    const int status = Api(L"GET", "/v1/me/tracks/contains?ids=" + trackId, {}, body);
    if (status != 200) return;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_array() || j.empty() || !j[0].is_boolean()) return;
    std::lock_guard lock(g_mutex);
    if (g_player.trackId == trackId) {
        g_player.liked = j[0].get<bool>();
        g_player.likedKnown = true;
        ++g_player.version;
    }
}

void FetchPlayer() {
    std::string body;
    const int status = Api(L"GET", "/v1/me/player", {}, body);
    if (status < 0) return;
    if (status == 204 || (status == 200 && body.empty())) {  // nothing playing anywhere
        std::lock_guard lock(g_mutex);
        const bool was = g_player.hasDevice;
        g_player.valid = true;
        g_player.hasDevice = false;
        g_player.playing = false;
        if (was) ++g_player.version;
        return;
    }
    if (status != 200) {
        if (status == 429) g_nextPoll = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return;
    }
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return;
    Player p;
    {
        std::lock_guard lock(g_mutex);
        p = g_player;
    }
    const std::string oldTrack = p.trackId;
    p.valid = true;
    p.hasDevice = j.contains("device") && j["device"].is_object();
    p.playing = j.value("is_playing", false);
    p.shuffle = j.value("shuffle_state", false);
    const std::string repeat = j.value("repeat_state", std::string("off"));
    p.repeat = repeat == "track" ? 1 : (repeat == "context" ? 2 : 0);
    p.progressMs = j.value("progress_ms", 0);
    p.at = std::chrono::steady_clock::now();
    if (p.hasDevice) {
        const auto& dv = j["device"];
        p.device.id = dv.value("id", std::string());
        p.device.name = dv.value("name", std::string());
        p.device.type = dv.value("type", std::string());
        p.device.active = true;
        p.device.volume = dv.contains("volume_percent") && dv["volume_percent"].is_number() ? dv["volume_percent"].get<int>() : -1;
        p.device.canVolume = dv.value("supports_volume", true);
    }
    const auto item = j.find("item");
    if (item != j.end() && item->is_object()) {
        p.trackId = item->value("id", std::string());
        p.title = item->value("name", std::string());
        p.durationMs = item->value("duration_ms", 0);
        p.artist = ArtistsOf(*item);
        if (item->contains("album") && (*item)["album"].is_object()) {
            const auto& album = (*item)["album"];
            p.album = album.value("name", std::string());
            if (p.trackId != oldTrack || p.artPath.empty()) {
                try {
                    p.artPath = album.contains("images") ? DownloadArt(album["images"]) : std::wstring();
                } catch (...) {
                    p.artPath.clear();
                }
            }
        } else if (item->contains("show") && (*item)["show"].is_object()) {  // (a podcast episode)
            p.album = (*item)["show"].value("name", std::string());
            p.artist = (*item)["show"].value("publisher", std::string());
            if (p.trackId != oldTrack) p.artPath = DownloadArt((*item)["images"]);
        }
    } else {
        p.trackId.clear();
        p.title.clear();
    }
    if (p.trackId != oldTrack) {
        p.likedKnown = false;
        p.liked = false;
    }
    ++p.version;
    {
        std::lock_guard lock(g_mutex);
        g_player = p;
        if (g_playerError == "no_device" && p.hasDevice) g_playerError.clear();
    }
    if (p.trackId != oldTrack && !p.trackId.empty()) {
        FetchLiked(p.trackId);
        FetchQueue();
    }
}

void FetchDevices() {
    std::string body;
    const int status = Api(L"GET", "/v1/me/player/devices", {}, body);
    if (status != 200) return;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("devices") || !j["devices"].is_array()) return;
    std::vector<Device> list;
    for (const auto& dv : j["devices"]) {
        if (!dv.is_object()) continue;
        Device d;
        d.id = dv.contains("id") && dv["id"].is_string() ? dv["id"].get<std::string>() : std::string();
        d.name = dv.value("name", std::string());
        d.type = dv.value("type", std::string());
        d.active = dv.value("is_active", false);
        d.volume = dv.contains("volume_percent") && dv["volume_percent"].is_number() ? dv["volume_percent"].get<int>() : -1;
        d.canVolume = dv.value("supports_volume", true);
        if (!d.id.empty()) list.push_back(std::move(d));
    }
    std::lock_guard lock(g_mutex);
    g_devices = std::move(list);
    ++g_devicesVersion;
}

void RunMix(const std::string& artistsCsv, const std::string& seedUri);  // (below)

void RunCommand(const Command& c) {
    std::string body;
    int status = -1;
    switch (c.kind) {
        case Command::Play: status = Api(L"PUT", "/v1/me/player/play", {}, body); break;
        case Command::Pause: status = Api(L"PUT", "/v1/me/player/pause", {}, body); break;
        case Command::Next: status = Api(L"POST", "/v1/me/player/next", {}, body); break;
        case Command::Previous: status = Api(L"POST", "/v1/me/player/previous", {}, body); break;
        case Command::Seek: status = Api(L"PUT", "/v1/me/player/seek?position_ms=" + std::to_string(c.value), {}, body); break;
        case Command::Shuffle: status = Api(L"PUT", std::string("/v1/me/player/shuffle?state=") + (c.value ? "true" : "false"), {}, body); break;
        case Command::Repeat: {
            const char* mode = c.value == 1 ? "track" : (c.value == 2 ? "context" : "off");
            status = Api(L"PUT", std::string("/v1/me/player/repeat?state=") + mode, {}, body);
            break;
        }
        case Command::Volume: status = Api(L"PUT", "/v1/me/player/volume?volume_percent=" + std::to_string(c.value), {}, body); break;
        case Command::Transfer: {
            const nlohmann::json j = {{"device_ids", {c.text}}, {"play", true}};
            status = Api(L"PUT", "/v1/me/player", j.dump(), body);
            break;
        }
        case Command::Like:
            status = Api(c.value ? L"PUT" : L"DELETE", "/v1/me/tracks?ids=" + c.text, {}, body);
            break;
        case Command::Raw: {
            const std::wstring method = Widen(c.method);
            status = Api(method.c_str(), c.text, c.body, body);
            break;
        }
        case Command::Mix:
            RunMix(c.text, c.body);
            status = 204;
            break;
    }
    if (status >= 200 && status < 300) {
        SetPlayerError({});
    } else if (status > 0) {
        const std::string reason = ReasonOf(status, body);
        log::Warn("Spotify: a command was refused ({}: {}).", status, reason);
        SetPlayerError(reason);
    }
    if (c.kind == Command::Transfer) g_devicesWanted.store(true);
    // what changed shows in Spotify's answer a moment later
    g_nextPoll = std::chrono::steady_clock::now() + std::chrono::milliseconds(c.kind == Command::Next || c.kind == Command::Previous ? 450 : 300);
}

// --- BROWSING (part S3): your library, search, pages ----------------------------------------
// The screens ask for Web API answers by their address ("/v1/me/playlists...").
// The first time, the address goes to a list; this thread asks Spotify (a
// couple per turn, never in the screen's frame) and keeps the parsed answer
// for a while. Pictures (covers, artists) are downloaded once to disk.

struct BrowseEntry {
    int state = 0;  // 0 asked, 1 here, 2 failed
    std::shared_ptr<const nlohmann::json> json;
    std::chrono::steady_clock::time_point at;
    std::uint32_t version = 0;
};
std::map<std::string, BrowseEntry> g_browse;   // (g_mutex)
std::deque<std::string> g_browseQueue;         // (g_mutex)
std::map<std::string, std::wstring> g_images;  // url -> file ("" = coming)  (g_mutex)
std::deque<std::string> g_imageQueue;          // (g_mutex)
std::uint32_t g_browseCounter = 0;
std::chrono::steady_clock::time_point g_browseWaitUntil;

std::wstring DownloadImage(const std::string& url) {
    const std::string prefix = "https://i.scdn.co";
    std::string host = "i.scdn.co";
    std::string path;
    if (url.rfind(prefix, 0) == 0) {
        path = url.substr(prefix.size());
    } else {
        // (some pictures live on other Spotify servers: mosaic.scdn.co, image-cdn-*.spotifycdn.com)
        const std::size_t scheme = url.find("://");
        if (scheme == std::string::npos) return {};
        const std::size_t slash = url.find('/', scheme + 3);
        if (slash == std::string::npos) return {};
        host = url.substr(scheme + 3, slash - scheme - 3);
        path = url.substr(slash);
        const bool spotifyHost = host.size() > 9 && (host.compare(host.size() - 9, 9, ".scdn.co") == 0 ||
                                                      host.find("spotifycdn.com") != std::string::npos);
        if (!spotifyHost) return {};
    }
    std::string name = host + path;
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    if (name.size() > 120) name = name.substr(name.size() - 120);
    const std::filesystem::path dir = paths::DataDir() / L"covers" / L"spotify";
    const std::filesystem::path file = dir / (Widen(name) + L".jpg");
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) return file.wstring();
    std::string data;
    if (Https(Widen(host).c_str(), L"GET", path, {}, {}, data) != 200 || data.size() < 100) return {};
    std::filesystem::create_directories(dir, ec);
    std::ofstream(file, std::ios::binary | std::ios::trunc).write(data.data(), static_cast<std::streamsize>(data.size()));
    return file.wstring();
}

// The mix ("radio"): the most played songs of these artists, mixed, after the seed.
void RunMix(const std::string& artistsCsv, const std::string& seedUri) {
    std::vector<std::string> uris;
    std::size_t pos = 0;
    int artists = 0;
    while (pos < artistsCsv.size() && artists < 6) {
        std::size_t end = artistsCsv.find(',', pos);
        if (end == std::string::npos) end = artistsCsv.size();
        const std::string id = artistsCsv.substr(pos, end - pos);
        pos = end + 1;
        if (id.empty()) continue;
        ++artists;
        std::string body;
        if (Api(L"GET", "/v1/artists/" + id + "/top-tracks?market=from_token", {}, body) != 200) continue;
        const auto j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_object() || !j.contains("tracks") || !j["tracks"].is_array()) continue;
        for (const auto& t : j["tracks"]) {
            if (t.is_object() && t.contains("uri") && t["uri"].is_string()) uris.push_back(t["uri"].get<std::string>());
        }
    }
    // mixed (and without repeats), the seed first
    std::sort(uris.begin(), uris.end());
    uris.erase(std::unique(uris.begin(), uris.end()), uris.end());
    std::uint32_t seed = static_cast<std::uint32_t>(GetTickCount64());
    for (std::size_t i = uris.size(); i > 1; --i) {
        seed = seed * 1664525u + 1013904223u;
        std::swap(uris[i - 1], uris[seed % i]);
    }
    if (!seedUri.empty()) {
        uris.erase(std::remove(uris.begin(), uris.end(), seedUri), uris.end());
        uris.insert(uris.begin(), seedUri);
    }
    if (uris.size() > 60) uris.resize(60);
    if (uris.empty()) {
        SetPlayerError("mix_empty");
        return;
    }
    const nlohmann::json play = {{"uris", uris}};
    std::string body;
    const int status = Api(L"PUT", "/v1/me/player/play", play.dump(), body);
    if (status >= 300) SetPlayerError(ReasonOf(status, body));
}

// LUMA'S OWN PLAYER (the launcher's hidden Edge page) needs the access pass
// too: this thread keeps a copy for it while it's asked for.
std::atomic<bool> g_tokenWanted{false};
std::string g_sharedToken;  // (g_mutex)

void ShareTokenTick() {
    if (!g_tokenWanted.load()) return;
    if (!EnsureToken()) return;
    std::lock_guard lock(g_mutex);
    g_sharedToken = g_accessToken;
}

void BrowseTick() {
    if (GetStatus().state != State::Connected) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_browseWaitUntil) return;
    for (int n = 0; n < 2; ++n) {  // two answers per turn
        std::string path;
        {
            std::lock_guard lock(g_mutex);
            if (g_browseQueue.empty()) break;
            path = g_browseQueue.front();
            g_browseQueue.pop_front();
        }
        std::string body;
        int status = -1;
        try {
            status = Api(L"GET", path, {}, body);
        } catch (...) {
        }
        if (status == 429) {  // too many: wait, and ask again later
            g_browseWaitUntil = now + std::chrono::seconds(3);
            std::lock_guard lock(g_mutex);
            g_browseQueue.push_back(path);
            return;
        }
        auto parsed = std::make_shared<nlohmann::json>(nlohmann::json::parse(body, nullptr, false));
        std::lock_guard lock(g_mutex);
        BrowseEntry& e = g_browse[path];
        e.at = std::chrono::steady_clock::now();
        e.version = ++g_browseCounter;
        if (status == 200 && !parsed->is_discarded()) {
            e.state = 1;
            e.json = std::move(parsed);
        } else {
            e.state = 2;
            if (status > 0) log::Info("Spotify: {} answered {}.", path.substr(0, 60), status);
        }
    }
    for (int n = 0; n < 3; ++n) {  // three pictures per turn
        std::string url;
        {
            std::lock_guard lock(g_mutex);
            if (g_imageQueue.empty()) break;
            url = g_imageQueue.front();
            g_imageQueue.pop_front();
        }
        std::wstring file;
        try {
            file = DownloadImage(url);
        } catch (...) {
        }
        std::lock_guard lock(g_mutex);
        g_images[url] = file.empty() ? L"-" : file;  // ("-" = it failed: not asked again)
    }
}

// One turn of the player's work (from the thread).
void PlayerTick() {
    std::vector<Command> commands;
    {
        std::lock_guard lock(g_mutex);
        commands.swap(g_commands);
        if (g_status.state != State::Connected) commands.clear();
    }
    for (const Command& c : commands) {
        try {
            RunCommand(c);
        } catch (...) {
        }
    }
    BrowseTick();
    ShareTokenTick();
    if (!g_playerWanted.load() || GetStatus().state != State::Connected) return;
    if (g_devicesWanted.exchange(false)) {
        try {
            FetchDevices();
        } catch (...) {
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + std::chrono::milliseconds(1000);
    try {
        FetchPlayer();
    } catch (...) {
    }
}

void Push(Command c) {
    {
        std::lock_guard lock(g_mutex);
        // a newer seek / volume replaces one that's still waiting
        if (c.kind == Command::Seek || c.kind == Command::Volume) {
            for (Command& old : g_commands) {
                if (old.kind == c.kind) {
                    old = c;
                    return;
                }
            }
        }
        g_commands.push_back(std::move(c));
    }
    EnsureThread();
}

void ThreadMain() {
    std::uint32_t lastTrack = 0;
    auto nextRefresh = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // (short: the player's buttons answer quickly)
        PlayerTick();
        if (g_disconnectRequested.exchange(false)) {
            g_profileAsked = false;
            DeletePass();
            g_accessToken.clear();
            PublishQueue({});
            SetState(ClientId().empty() ? State::NoClientId : State::SignedOut);
            continue;
        }
        if (g_connectRequested.exchange(false)) {
            RunConnect();
            lastTrack = 0;  // ask for the queue right away
            g_profileAsked = false;
        }
        {
            const bool outdated = ScopeOutdated();
            std::lock_guard lock(g_mutex);
            g_status.outdated = outdated && g_status.state == State::Connected;
            if (g_status.state != State::Connected) {
                g_status.user.clear();
                g_status.premium = -1;
            }
        }
        if (!g_profileAsked && std::filesystem::exists(PassPath()) && !ClientId().empty()) {
            try {
                FetchProfile();
            } catch (...) {
            }
            g_profileAsked = true;  // (once per connection; a failure isn't retried in a loop)
        }
        if (!g_active.load()) continue;
        const media::Track t = media::Current();
        // The queue changes when the song changes (and when you add songs:
        // checked again every 30 s while the same song plays).
        const auto now = std::chrono::steady_clock::now();
        if (!t.present || (t.trackId == lastTrack && now < nextRefresh)) continue;
        if (t.app[0] != '\0' && std::strstr(t.app, "Spotify") == nullptr) continue;  // only when Spotify plays
        lastTrack = t.trackId;
        nextRefresh = now + std::chrono::seconds(30);
        try {
            FetchQueue();
        } catch (...) {
        }
    }
}

void EnsureThread() {
    std::lock_guard lock(g_threadMutex);
    if (!g_thread.joinable() && !g_stop.load()) g_thread = std::thread(ThreadMain);
}

}  // namespace

void SetClientId(const std::string& clientId) {
    bool changed = false;
    {
        std::lock_guard lock(g_mutex);
        changed = g_clientId != clientId;
        g_clientId = clientId;
        if (clientId.empty()) {
            g_status.state = State::NoClientId;
        } else if (g_status.state == State::NoClientId) {
            g_status.state = std::filesystem::exists(PassPath()) ? State::Connected : State::SignedOut;
        }
    }
    if (changed && !clientId.empty()) EnsureThread();
}

void SetActive(bool active) {
    g_active.store(active);
    if (active) EnsureThread();
}

void SetPrefetch(bool on) {
    g_prefetch.store(on);
}

void Connect() {
    if (ClientId().empty()) return;
    g_connectRequested.store(true);
    EnsureThread();
}

void Disconnect() {
    g_disconnectRequested.store(true);
    EnsureThread();
}

Status GetStatus() {
    std::lock_guard lock(g_mutex);
    return g_status;
}

bool Queue(std::uint32_t knownVersion, std::vector<QueueItem>& out, std::uint32_t& version) {
    std::lock_guard lock(g_mutex);
    if (g_queueVersion == knownVersion) return false;
    out = g_queue;
    version = g_queueVersion;
    return true;
}

// --- The player (see spotify.h) -------------------------------------------------------------

void SetPlayerWanted(bool wanted) {
    if (wanted && !g_playerWanted.load()) g_nextPoll = {};  // (ask right away)
    g_playerWanted.store(wanted);
    if (wanted) EnsureThread();
}

Player GetPlayer() {
    std::lock_guard lock(g_mutex);
    return g_player;
}

double PositionSeconds(const Player& p) {
    double ms = p.progressMs;
    if (p.playing) ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p.at).count();
    if (p.durationMs > 0) ms = std::min(ms, static_cast<double>(p.durationMs));
    return ms / 1000.0;
}

std::string PlayerError() {
    std::lock_guard lock(g_mutex);
    // (an error is shown for a while, then forgotten)
    if (!g_playerError.empty() && std::chrono::steady_clock::now() - g_playerErrorAt > std::chrono::seconds(8) && g_playerError != "no_device") {
        g_playerError.clear();
    }
    return g_playerError;
}

void RequestDevices() {
    g_devicesWanted.store(true);
    EnsureThread();
}

bool Devices(std::uint32_t knownVersion, std::vector<Device>& out, std::uint32_t& version) {
    std::lock_guard lock(g_mutex);
    if (g_devicesVersion == knownVersion) return false;
    out = g_devices;
    version = g_devicesVersion;
    return true;
}

// The commands: the screen changes at once (Spotify confirms a moment later).
void PlayerPlay(bool play) {
    {
        std::lock_guard lock(g_mutex);
        g_player.progressMs = static_cast<int>(PositionSeconds(g_player) * 1000.0);
        g_player.at = std::chrono::steady_clock::now();
        g_player.playing = play;
        ++g_player.version;
    }
    Push({play ? Command::Play : Command::Pause, 0, {}});
}

void PlayerNext() {
    Push({Command::Next, 0, {}});
}

void PlayerPrevious() {
    Push({Command::Previous, 0, {}});
}

void PlayerSeek(double seconds) {
    const long long ms = static_cast<long long>(std::max(0.0, seconds) * 1000.0);
    {
        std::lock_guard lock(g_mutex);
        g_player.progressMs = static_cast<int>(ms);
        g_player.at = std::chrono::steady_clock::now();
        ++g_player.version;
    }
    Push({Command::Seek, ms, {}});
}

void PlayerShuffle(bool on) {
    {
        std::lock_guard lock(g_mutex);
        g_player.shuffle = on;
        ++g_player.version;
    }
    Push({Command::Shuffle, on ? 1 : 0, {}});
}

void PlayerRepeat(int mode) {
    mode = std::clamp(mode, 0, 2);
    {
        std::lock_guard lock(g_mutex);
        g_player.repeat = mode;
        ++g_player.version;
    }
    Push({Command::Repeat, mode, {}});
}

void PlayerVolume(int percent) {
    percent = std::clamp(percent, 0, 100);
    {
        std::lock_guard lock(g_mutex);
        g_player.device.volume = percent;
        ++g_player.version;
    }
    Push({Command::Volume, percent, {}});
}

void PlayerTransfer(const std::string& deviceId) {
    Push({Command::Transfer, 0, deviceId});
}

void PlayerLike(bool liked) {
    std::string id;
    {
        std::lock_guard lock(g_mutex);
        id = g_player.trackId;
        g_player.liked = liked;
        g_player.likedKnown = true;
        ++g_player.version;
    }
    if (!id.empty()) Push({Command::Like, liked ? 1 : 0, id});
}

// --- Browsing (see spotify.h) -----------------------------------------------------------------

int Browse(const std::string& path, std::shared_ptr<const nlohmann::json>& out, int maxAgeSeconds) {
    std::lock_guard lock(g_mutex);
    auto it = g_browse.find(path);
    const auto now = std::chrono::steady_clock::now();
    if (it == g_browse.end()) {
        g_browse[path] = BrowseEntry{};
        g_browseQueue.push_back(path);
        return 0;
    }
    BrowseEntry& e = it->second;
    if (e.state != 0 && now - e.at > std::chrono::seconds(maxAgeSeconds)) {
        // old: ask again (and keep showing the old answer meanwhile)
        e.at = now;
        if (std::find(g_browseQueue.begin(), g_browseQueue.end(), path) == g_browseQueue.end()) g_browseQueue.push_back(path);
    }
    if (e.json) out = e.json;
    return e.state == 1 || e.json ? 1 : e.state;
}

void Forget(const std::string& prefix) {
    std::lock_guard lock(g_mutex);
    for (auto it = g_browse.begin(); it != g_browse.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = g_browse.erase(it);
        } else {
            ++it;
        }
    }
}

std::wstring Image(const std::string& url) {
    if (url.empty()) return {};
    std::lock_guard lock(g_mutex);
    auto it = g_images.find(url);
    if (it == g_images.end()) {
        g_images[url] = L"";
        g_imageQueue.push_back(url);
        return {};
    }
    return it->second == L"-" ? std::wstring() : it->second;
}

void Send(const char* method, const std::string& path, const std::string& body) {
    Command c{Command::Raw, 0, path};
    c.method = method;
    c.body = body;
    Push(std::move(c));
}

void PlayMix(const std::vector<std::string>& artistIds, const std::string& seedUri) {
    std::string csv;
    for (const std::string& id : artistIds) {
        if (id.empty()) continue;
        if (!csv.empty()) csv += ',';
        csv += id;
    }
    Command c{Command::Mix, 0, csv};
    c.body = seedUri;
    Push(std::move(c));
}

std::string SharedToken() {
    g_tokenWanted.store(true);
    EnsureThread();
    std::lock_guard lock(g_mutex);
    return g_sharedToken;
}

void Shutdown() {
    g_stop.store(true);
    std::lock_guard lock(g_threadMutex);
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace luma::spotify
