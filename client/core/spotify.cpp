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

#include <atomic>
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
constexpr const char* kScope = "user-read-playback-state user-read-currently-playing";

std::mutex g_mutex;          // protects everything below that's not atomic
std::string g_clientId;
Status g_status;
std::vector<QueueItem> g_queue;
std::uint32_t g_queueVersion = 0;

std::atomic<bool> g_active{false};
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
    const int r = Redeem("grant_type=refresh_token&refresh_token=" + UrlEncode(pass) + "&client_id=" + UrlEncode(clientId));
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
    for (std::size_t i = 0; i < items.size() && i < 3; ++i) {
        lyrics::Prefetch(items[i].title, items[i].artist, items[i].album, items[i].seconds);
    }
    PublishQueue(std::move(items));
}

void ThreadMain() {
    std::uint32_t lastTrack = 0;
    auto nextRefresh = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (g_disconnectRequested.exchange(false)) {
            DeletePass();
            g_accessToken.clear();
            PublishQueue({});
            SetState(ClientId().empty() ? State::NoClientId : State::SignedOut);
            continue;
        }
        if (g_connectRequested.exchange(false)) {
            RunConnect();
            lastTrack = 0;  // ask for the queue right away
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

void Shutdown() {
    g_stop.store(true);
    std::lock_guard lock(g_threadMutex);
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace luma::spotify
