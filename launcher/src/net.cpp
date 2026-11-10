// =============================================================================
//  net.cpp - Pedidos HTTPS (ver net.h)
// =============================================================================
#include "net.h"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <vector>

namespace luma::launcher::net {

namespace {

// Abre la conexión y manda el pedido. Devuelve los tres "handles" de WinHTTP
// (sesión, conexión, pedido) o false. Las redirecciones (por ejemplo, de
// github.com a su servidor de descargas) las sigue WinHTTP solo.
struct Handles {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~Handles() {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

bool Open(Handles& h, const wchar_t* method, const std::wstring& url, const std::string& body, const std::wstring& headers) {
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;  // solo HTTPS
    }
    const std::wstring fullPath = std::wstring(path) + extra;
    h.session = WinHttpOpen(L"LumaLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (h.session == nullptr) return false;
    WinHttpSetTimeouts(h.session, 10000, 10000, 15000, 30000);
    h.connection = WinHttpConnect(h.session, host, parts.nPort, 0);
    if (h.connection == nullptr) return false;
    h.request = WinHttpOpenRequest(h.connection, method, fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (h.request == nullptr) return false;
    return WinHttpSendRequest(h.request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                              headers.empty() ? 0 : static_cast<DWORD>(-1L),
                              body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                              static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
           WinHttpReceiveResponse(h.request, nullptr);
}

int Status(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return static_cast<int>(status);
}

std::uint64_t ContentLength(HINTERNET request) {
    wchar_t text[32]{};
    DWORD size = sizeof(text);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return _wcstoui64(text, nullptr, 10);
}

}  // namespace

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool Request(const wchar_t* method, const std::wstring& url, const std::string& body, const std::wstring& headers,
             Response& out, std::size_t maxBytes) {
    Handles h;
    if (!Open(h, method, url, body, headers)) {
        return false;
    }
    out.status = Status(h.request);
    out.body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(h.request, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(h.request, chunk.data(), available, &read) || read == 0) break;
        out.body.append(chunk.data(), read);
        if (out.body.size() > maxBytes) {
            return false;  // más grande de lo razonable: no seguir
        }
    }
    return true;
}

int Download(const std::wstring& url, const std::filesystem::path& file,
             const std::function<void(std::uint64_t, std::uint64_t)>& progress, const std::atomic<bool>& cancel,
             std::uint64_t maxBytes) {
    Handles h;
    if (!Open(h, L"GET", url, {}, L"")) {
        return 0;
    }
    const int status = Status(h.request);
    if (status != 200) {
        return status;
    }
    const std::uint64_t total = ContentLength(h.request);
    if (total > maxBytes) {
        return 0;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return 0;
    }
    std::vector<char> buffer(64 * 1024);
    std::uint64_t done = 0;
    for (;;) {
        if (cancel.load()) {
            return 0;
        }
        DWORD read = 0;
        if (!WinHttpReadData(h.request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            return 0;
        }
        if (read == 0) break;  // terminó
        out.write(buffer.data(), read);
        done += read;
        if (done > maxBytes) {
            return 0;
        }
        if (progress) progress(done, total);
    }
    out.close();
    return out ? 200 : 0;
}

}  // namespace luma::launcher::net
