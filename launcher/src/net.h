// =============================================================================
//  net.h - Pedidos HTTPS (con WinHTTP, la biblioteca de Windows)
// =============================================================================
//  Lo usan el inicio de sesión (account.cpp) y el actualizador (updater.cpp).
//  Solo HTTPS: una conexión cifrada, así nadie en el camino puede leer ni
//  cambiar lo que se envía o se recibe.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace luma::launcher::net {

struct Response {
    int status = 0;     // 200 = bien, 404 = no existe...
    std::string body;
};

// Un pedido completo en memoria (respuestas chicas: JSON, firmas...). "headers"
// = encabezados extra separados por "\r\n". false si no hubo conexión.
bool Request(const wchar_t* method, const std::wstring& url, const std::string& body, const std::wstring& headers,
             Response& out, std::size_t maxBytes = 8 * 1024 * 1024);

// Descarga a un archivo. "progress(bajado, total)" se llama mientras baja
// (total = 0 si el servidor no lo dice). Se puede cancelar con "cancel".
// Devuelve el código HTTP (0 si no hubo conexión o se canceló).
int Download(const std::wstring& url, const std::filesystem::path& file,
             const std::function<void(std::uint64_t, std::uint64_t)>& progress, const std::atomic<bool>& cancel,
             std::uint64_t maxBytes);

std::wstring Widen(const std::string& text);

}  // namespace luma::launcher::net
