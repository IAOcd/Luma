// =============================================================================
//  account.cpp - Inicio de sesión con Microsoft / Xbox (ver account.h)
// =============================================================================
//  Bibliotecas de Windows que usamos (ninguna externa):
//    - WinHTTP: hablar con los servidores (HTTPS).
//    - Winsock: el mini servidor en localhost que recibe el código.
//    - BCrypt: números aleatorios y SHA-256 (para PKCE).
//    - Crypt32 (DPAPI): cifrar la sesión guardada.
//    - WIC: decodificar la foto de perfil (PNG).
// =============================================================================
#include <winsock2.h>  // antes que windows.h
#include <ws2tcpip.h>

#include "account.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>  // SHCreateMemStream
#include <wincodec.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace luma::launcher::account {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// El identificador de "Luma Launcher" en Azure (ver account.h).
constexpr const char* kClientId = "602c60cc-fed7-4ec9-aa25-f9f73ca0959b";
// "consumers" = solo cuentas personales de Microsoft (las de Xbox).
constexpr const wchar_t* kAuthorizeUrl = L"https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize";
constexpr const wchar_t* kTokenUrl = L"https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr const wchar_t* kXblUrl = L"https://user.auth.xboxlive.com/user/authenticate";
constexpr const wchar_t* kXstsUrl = L"https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr const wchar_t* kProfileUrl =
    L"https://profile.xboxlive.com/users/me/profile/settings?settings=GameDisplayPicRaw,Gamertag";
constexpr const char* kScope = "XboxLive.signin offline_access";
constexpr auto kBrowserTimeout = std::chrono::minutes(4);

std::mutex g_mutex;
Info g_info;
std::thread g_worker;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};

// --- Pequeñas ayudas ------------------------------------------------------------------
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char hex[4];
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
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// Un parámetro de "a=1&b=2".
std::string QueryParam(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        const std::size_t end = std::min(query.find('&', pos), query.size());
        const std::string pair = query.substr(pos, end - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
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

void SetState(State state, const char* errorKey = "", std::string detail = {}) {
    std::lock_guard lock(g_mutex);
    g_info.state = state;
    g_info.errorKey = errorKey;
    g_info.detail = std::move(detail);
}

// --- HTTPS con WinHTTP --------------------------------------------------------------------
struct Response {
    int status = 0;
    std::string body;
};

bool Http(const wchar_t* method, const std::wstring& url, const std::string& body, const std::wstring& headers,
          Response& out) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    wchar_t extra[2048]{};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        return false;
    }
    const std::wstring fullPath = std::wstring(path) + extra;
    HINTERNET session = WinHttpOpen(L"LumaLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection != nullptr
                            ? WinHttpOpenRequest(connection, method, fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr;
    bool ok = request != nullptr &&
              WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        out.status = static_cast<int>(status);
        out.body.clear();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            out.body.append(chunk.data(), read);
            if (out.body.size() > 8 * 1024 * 1024) break;  // nada razonable pesa tanto
        }
    }
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

bool PostJson(const wchar_t* url, const json& body, const std::wstring& extraHeaders, Response& out) {
    const std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n" + extraHeaders;
    return Http(L"POST", url, body.dump(), headers, out);
}

// --- La sesión guardada (cifrada con DPAPI) ---------------------------------------------
fs::path SessionPath() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma" / L"account.dat";
    }
    CoTaskMemFree(raw);
    return path;
}

void SaveRefreshToken(const std::string& token) {
    DATA_BLOB in{static_cast<DWORD>(token.size()), reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Luma", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return;
    }
    const fs::path path = SessionPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
}

std::string LoadRefreshToken() {
    std::ifstream file(SessionPath(), std::ios::binary);
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
    return token;
}

void DeleteSession() {
    std::error_code ec;
    fs::remove(SessionPath(), ec);
}

// --- Paso 1: el navegador y el mini servidor en localhost ------------------------------
// Devuelve la "query" que llegó (code=...&state=...) o vacío si se canceló/venció.
std::string WaitForRedirect(SOCKET listener) {
    const auto deadline = std::chrono::steady_clock::now() + kBrowserTimeout;
    while (!g_cancel.load() && std::chrono::steady_clock::now() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval wait{0, 250000};  // revisar "cancelar" cada 1/4 de segundo
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) {
            continue;
        }
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        char buffer[8192];
        const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        std::string request(buffer, received > 0 ? received : 0);
        // Primera línea: "GET /?code=...&state=... HTTP/1.1"
        std::string query;
        const std::size_t q = request.find('?');
        const std::size_t end = request.find(' ', q == std::string::npos ? 0 : q);
        if (request.rfind("GET /", 0) == 0 && q != std::string::npos && end != std::string::npos && q < end) {
            query = request.substr(q + 1, end - q - 1);
        }
        const bool useful = !QueryParam(query, "code").empty() || !QueryParam(query, "error").empty();
        const char* page =
            "<!doctype html><html><head><meta charset=\"utf-8\"><title>Luma</title><style>"
            "body{background:#1B1D1F;color:#E9E6DF;font-family:Segoe UI,sans-serif;display:flex;"
            "align-items:center;justify-content:center;height:100vh;margin:0}"
            "div{text-align:center}h1{font-weight:600}p{opacity:.6}</style></head><body><div>"
            "<h1>Luma</h1><p>You can close this tab and go back to the launcher.</p>"
            "<p>Ya puedes cerrar esta pestaña y volver al launcher.</p></div></body></html>";
        char response[2048];
        std::snprintf(response, sizeof(response),
                      "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n\r\n%s",
                      useful ? "200 OK" : "404 Not Found", useful ? std::strlen(page) : 0, useful ? page : "");
        send(client, response, static_cast<int>(std::strlen(response)), 0);
        closesocket(client);
        if (useful) {
            return query;
        }
    }
    return {};
}

// --- Pasos 2 y 3: tokens ---------------------------------------------------------------------
struct XboxResult {
    bool ok = false;
    const char* errorKey = "account.err_generic";
    std::string detail;
    std::string gamertag;
    std::string authHeader;  // "XBL3.0 x=<uhs>;<token>" para pedir la foto
};

// Token de Microsoft -> Xbox Live -> XSTS (que trae el gamertag).
XboxResult XboxSignIn(const std::string& msAccessToken) {
    XboxResult result;
    Response r;
    const json xblBody = {{"Properties",
                           {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"}, {"RpsTicket", "d=" + msAccessToken}}},
                          {"RelyingParty", "http://auth.xboxlive.com"},
                          {"TokenType", "JWT"}};
    if (!PostJson(kXblUrl, xblBody, L"x-xbl-contract-version: 1\r\n", r)) {
        result.errorKey = "account.err_network";
        return result;
    }
    const json xbl = json::parse(r.body, nullptr, false);
    if (r.status != 200 || !xbl.is_object() || !xbl.contains("Token")) {
        result.detail = "Xbox Live " + std::to_string(r.status);
        return result;
    }
    const json xstsBody = {{"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", {xbl["Token"]}}}},
                           {"RelyingParty", "http://xboxlive.com"},
                           {"TokenType", "JWT"}};
    if (!PostJson(kXstsUrl, xstsBody, L"x-xbl-contract-version: 1\r\n", r)) {
        result.errorKey = "account.err_network";
        return result;
    }
    const json xsts = json::parse(r.body, nullptr, false);
    if (r.status == 401 && xsts.is_object()) {
        // Errores conocidos de Xbox (número XErr):
        const std::uint64_t code = xsts.value("XErr", static_cast<std::uint64_t>(0));
        result.errorKey = code == 2148916233ull   ? "account.err_no_xbox"   // la cuenta no tiene perfil de Xbox
                          : code == 2148916238ull ? "account.err_child"     // cuenta de menor: necesita un adulto
                                                  : "account.err_generic";
        result.detail = "XErr " + std::to_string(code);
        return result;
    }
    if (r.status != 200 || !xsts.is_object() || !xsts.contains("Token")) {
        result.detail = "XSTS " + std::to_string(r.status);
        return result;
    }
    try {
        const json& claims = xsts["DisplayClaims"]["xui"][0];
        result.gamertag = claims.value("gtg", std::string());
        result.authHeader = "XBL3.0 x=" + claims.value("uhs", std::string()) + ";" + xsts["Token"].get<std::string>();
    } catch (...) {
        result.detail = "XSTS claims";
        return result;
    }
    result.ok = !result.gamertag.empty();
    return result;
}

// Decodifica una imagen (PNG/JPG) a RGBA de 128x128 con WIC.
bool DecodePicture(const std::string& bytes, std::vector<std::uint8_t>& rgba, int& width, int& height) {
    IWICImagingFactory* factory = nullptr;
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    constexpr UINT kSize = 128;
    bool ok = stream != nullptr &&
              SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
              SUCCEEDED(scaler->Initialize(frame, kSize, kSize, WICBitmapInterpolationModeFant)) &&
              SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom));
    if (ok) {
        rgba.resize(kSize * kSize * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, kSize * 4, static_cast<UINT>(rgba.size()), rgba.data()));
        width = height = static_cast<int>(kSize);
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

void FetchPicture(const std::string& authHeader) {
    Response r;
    const std::wstring headers = L"Authorization: " + Widen(authHeader) + L"\r\nx-xbl-contract-version: 2\r\nAccept: application/json\r\n";
    if (!Http(L"GET", kProfileUrl, {}, headers, r) || r.status != 200) {
        return;
    }
    const json profile = json::parse(r.body, nullptr, false);
    std::string url;
    try {
        for (const json& setting : profile["profileUsers"][0]["settings"]) {
            if (setting.value("id", std::string()) == "GameDisplayPicRaw") {
                url = setting.value("value", std::string());
            }
        }
    } catch (...) {
        return;
    }
    if (url.rfind("https://", 0) != 0) {
        return;  // solo HTTPS
    }
    url += url.find('?') == std::string::npos ? "?" : "&";
    url += "format=png&w=128&h=128";
    if (!Http(L"GET", Widen(url), {}, L"", r) || r.status != 200 || r.body.empty()) {
        return;
    }
    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    if (DecodePicture(r.body, rgba, w, h)) {
        std::lock_guard lock(g_mutex);
        g_info.picture = std::move(rgba);
        g_info.pictureWidth = w;
        g_info.pictureHeight = h;
        ++g_info.pictureVersion;
    }
}

// Canjea en el servidor de tokens (un código nuevo, o el refresh token guardado).
bool RedeemToken(const std::string& form, std::string& accessToken, std::string& errorDetail, bool& network) {
    Response r;
    network = false;
    if (!Http(L"POST", kTokenUrl, form, L"Content-Type: application/x-www-form-urlencoded\r\n", r)) {
        network = true;
        return false;
    }
    const json token = json::parse(r.body, nullptr, false);
    if (r.status != 200 || !token.is_object() || !token.contains("access_token")) {
        errorDetail = token.is_object() ? token.value("error", std::string("token")) : "token";
        return false;
    }
    accessToken = token["access_token"].get<std::string>();
    if (token.contains("refresh_token")) {
        SaveRefreshToken(token["refresh_token"].get<std::string>());
    }
    return true;
}

// Después de tener el token de Microsoft: Xbox, gamertag y foto.
void Finish(const std::string& accessToken, bool silent) {
    const XboxResult xbox = XboxSignIn(accessToken);
    if (!xbox.ok) {
        if (silent && std::strcmp(xbox.errorKey, "account.err_network") == 0) {
            SetState(State::SignedOut);  // sin internet al abrir: no es un error para mostrar
        } else {
            SetState(State::Error, xbox.errorKey, xbox.detail);
        }
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_info.gamertag = xbox.gamertag;
        g_info.state = State::SignedIn;
        g_info.errorKey.clear();
        g_info.detail.clear();
    }
    FetchPicture(xbox.authHeader);
}

void RunSignIn() {
    SetState(State::SigningIn);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // El mini servidor: 127.0.0.1, puerto libre que elige Windows.
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof(address);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 4) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        SetState(State::Error, "account.err_generic", "localhost");
        return;
    }
    const std::string redirect = "http://localhost:" + std::to_string(ntohs(address.sin_port));
    const std::string verifier = RandomToken(48);
    const std::string state = RandomToken(16);
    const std::string url = "?client_id=" + std::string(kClientId) + "&response_type=code&response_mode=query" +
                            "&redirect_uri=" + UrlEncode(redirect) + "&scope=" + UrlEncode(kScope) +
                            "&code_challenge=" + Sha256Base64Url(verifier) + "&code_challenge_method=S256" +
                            "&state=" + state + "&prompt=select_account";
    const std::wstring full = std::wstring(kAuthorizeUrl) + Widen(url);
    ShellExecuteW(nullptr, L"open", full.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    const std::string query = WaitForRedirect(listener);
    closesocket(listener);
    WSACleanup();
    if (g_cancel.load()) {
        SetState(State::SignedOut);
        return;
    }
    if (query.empty()) {
        SetState(State::Error, "account.err_timeout");
        return;
    }
    if (QueryParam(query, "state") != state) {
        SetState(State::Error, "account.err_generic", "state");  // no es la respuesta a NUESTRO pedido
        return;
    }
    const std::string code = QueryParam(query, "code");
    if (code.empty()) {
        const std::string error = QueryParam(query, "error");
        SetState(error == "access_denied" ? State::SignedOut : State::Error,
                 error == "access_denied" ? "" : "account.err_generic", error);
        return;
    }
    const std::string form = "client_id=" + std::string(kClientId) + "&grant_type=authorization_code&code=" +
                             UrlEncode(code) + "&redirect_uri=" + UrlEncode(redirect) +
                             "&code_verifier=" + verifier + "&scope=" + UrlEncode(kScope);
    std::string accessToken;
    std::string detail;
    bool network = false;
    if (!RedeemToken(form, accessToken, detail, network)) {
        SetState(State::Error, network ? "account.err_network" : "account.err_generic", detail);
        return;
    }
    Finish(accessToken, false);
}

void RunRefresh() {
    const std::string refresh = LoadRefreshToken();
    if (refresh.empty()) {
        SetState(State::SignedOut);
        return;
    }
    SetState(State::Refreshing);
    const std::string form = "client_id=" + std::string(kClientId) + "&grant_type=refresh_token&refresh_token=" +
                             UrlEncode(refresh) + "&scope=" + UrlEncode(kScope);
    std::string accessToken;
    std::string detail;
    bool network = false;
    if (!RedeemToken(form, accessToken, detail, network)) {
        if (!network) {
            DeleteSession();  // la sesión venció o la revocaste: hay que volver a iniciar
        }
        SetState(State::SignedOut);
        return;
    }
    Finish(accessToken, true);
}

template <typename F>
void RunInBackground(F work) {
    if (g_busy.exchange(true)) {
        return;
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_cancel.store(false);
    g_worker = std::thread([work] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // para WIC
        work();
        CoUninitialize();
        g_busy.store(false);
    });
}

}  // namespace

void Init() {
    RunInBackground(RunRefresh);
}

void SignIn() {
    RunInBackground(RunSignIn);
}

void Cancel() {
    g_cancel.store(true);
}

void SignOut() {
    Cancel();
    DeleteSession();
    std::lock_guard lock(g_mutex);
    g_info = Info{};
    g_info.pictureVersion = -1;  // (para que la interfaz suelte la textura)
}

Info Get(bool includePicture) {
    std::lock_guard lock(g_mutex);
    if (includePicture) {
        return g_info;
    }
    Info copy;  // sin la foto (copiarla cada frame sería un desperdicio)
    copy.state = g_info.state;
    copy.gamertag = g_info.gamertag;
    copy.errorKey = g_info.errorKey;
    copy.detail = g_info.detail;
    copy.pictureWidth = g_info.pictureWidth;
    copy.pictureHeight = g_info.pictureHeight;
    copy.pictureVersion = g_info.pictureVersion;
    return copy;
}

void Shutdown() {
    g_cancel.store(true);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

}  // namespace luma::launcher::account
