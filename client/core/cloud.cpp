// =============================================================================
//  cloud.cpp - Conexión con el servidor de Luma (ver cloud.h)
// =============================================================================
#include "core/cloud.h"

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/paths.h"

namespace luma::cloud {

namespace {

constexpr const wchar_t* kHost = L"luma-api.gios-app.workers.dev";
constexpr ULONGLONG kPresenceEveryMs = 5ull * 60 * 1000;
constexpr ULONGLONG kLookupCacheMs = 3ull * 60 * 1000;

std::thread g_thread;
std::atomic<bool> g_quit{false};
std::mutex g_mutex;
std::condition_variable g_wake;
std::string g_token;                                  // el pase de Luma ("" = sin sesión)
std::vector<std::string> g_pending;                   // nombres para preguntar
std::map<std::string, ULONGLONG> g_asked;             // nombre (minúsculas) -> cuándo se preguntó
std::set<std::string> g_luma;                         // nombres (minúsculas) que usan Luma

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Lee el pase que dejó el launcher (cifrado con DPAPI: solo tu usuario de Windows lo abre).
std::string LoadToken() {
    std::ifstream file(paths::DataDir() / L"session.dat", std::ios::binary);
    if (!file) return {};
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    DATA_BLOB in{static_cast<DWORD>(data.size()), reinterpret_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (data.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    std::string token(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return token.size() == 64 ? token : std::string();
}

// Un pedido HTTPS al servidor. Devuelve el código (0 = sin conexión).
int Post(const wchar_t* path, const std::string& body, const std::string& token, std::string* response) {
    HINTERNET session = WinHttpOpen(L"Luma/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection != nullptr ? WinHttpOpenRequest(connection, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                              : nullptr;
    int status = 0;
    if (request != nullptr) {
        std::wstring headers = L"Content-Type: application/json\r\n";
        if (!token.empty()) headers += L"Authorization: Bearer " + std::wstring(token.begin(), token.end()) + L"\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0;
            DWORD size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
            if (response != nullptr) {
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || available == 0 || response->size() > 256 * 1024) break;
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                    response->append(chunk.data(), read);
                }
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status;
}

void Worker() {
    const std::string token = LoadToken();
    {
        std::lock_guard lock(g_mutex);
        g_token = token;
    }
    log::Info("Nube: {}", token.empty() ? "sin pase de Luma (inicia sesión con Microsoft en el launcher para aparecer como usuario de Luma)."
                                        : "pase de Luma encontrado.");
    ULONGLONG lastPresence = 0;
    bool loggedPresence = false;
    while (!g_quit.load()) {
        // 1. "Sigo usando Luma" (al arrancar y cada 5 minutos).
        if (!token.empty() && (lastPresence == 0 || GetTickCount64() - lastPresence >= kPresenceEveryMs)) {
            lastPresence = GetTickCount64();
            const int status = Post(L"/v1/presence", "{}", token, nullptr);
            if (!loggedPresence) {
                loggedPresence = true;
                log::Info("Nube: aviso de presencia -> {}", status == 200 ? "ok" : std::to_string(status));
            }
        }
        // 2. Preguntar por los nombres pendientes (de a muchos juntos).
        std::vector<std::string> names;
        {
            std::lock_guard lock(g_mutex);
            names.swap(g_pending);
        }
        if (!names.empty()) {
            std::string response;
            const nlohmann::json body{{"names", names}};
            if (Post(L"/v1/lookup", body.dump(), {}, &response) == 200) {
                const nlohmann::json reply = nlohmann::json::parse(response, nullptr, false);
                std::lock_guard lock(g_mutex);
                for (const std::string& n : names) g_luma.erase(Lower(n));  // los que no vuelven, ya no usan Luma
                if (reply.is_object() && reply.contains("users") && reply["users"].is_array()) {
                    for (const auto& u : reply["users"]) {
                        if (u.is_object()) g_luma.insert(Lower(u.value("gamertag", std::string())));
                    }
                }
            }
        }
        std::unique_lock lock(g_mutex);
        g_wake.wait_for(lock, std::chrono::seconds(2), [] { return g_quit.load() || !g_pending.empty(); });
    }
}

}  // namespace

void Start() {
    g_quit.store(false);
    g_thread = std::thread(Worker);
}

void Stop() {
    g_quit.store(true);
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void RequestLookup(const std::vector<std::string>& names) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    bool added = false;
    for (const std::string& n : names) {
        const std::string key = Lower(n);
        auto it = g_asked.find(key);
        if (it != g_asked.end() && now - it->second < kLookupCacheMs) continue;
        g_asked[key] = now;
        g_pending.push_back(n);
        added = true;
    }
    if (g_pending.size() > 120) g_pending.resize(120);
    if (added) g_wake.notify_all();
}

bool IsLumaUser(const std::string& name) {
    std::lock_guard lock(g_mutex);
    return g_luma.count(Lower(name)) != 0;
}

bool SignedIn() {
    std::lock_guard lock(g_mutex);
    return !g_token.empty();
}

}  // namespace luma::cloud
