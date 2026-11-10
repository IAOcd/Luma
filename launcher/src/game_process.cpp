// =============================================================================
//  process.cpp - Implementación de las herramientas de procesos
// =============================================================================
#include "game_process.h"

#include <tlhelp32.h>  // "Tool Help": listar procesos y módulos (DLLs) cargados

#include <cwchar>

namespace luma::launcher {

std::vector<DWORD> FindProcessesByName(std::wstring_view exeName) {
    std::vector<DWORD> pids;

    // Una "snapshot" es una foto de la lista de procesos en este instante.
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) {
        return pids;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);  // Windows exige que le digamos el tamaño.
    // Process32First/Next recorren la foto proceso por proceso.
    for (BOOL ok = Process32FirstW(snapshot.Get(), &entry); ok;
         ok = Process32NextW(snapshot.Get(), &entry)) {
        // _wcsicmp compara textos sin importar mayúsculas/minúsculas.
        if (_wcsicmp(entry.szExeFile, std::wstring(exeName).c_str()) == 0) {
            pids.push_back(entry.th32ProcessID);
        }
    }
    return pids;
}

std::optional<std::filesystem::path> GetProcessImagePath(HANDLE process) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        return std::nullopt;
    }
    return std::filesystem::path(std::wstring(buffer, size));
}

std::optional<bool> IsAppContainer(HANDLE process) {
    // Cada proceso tiene un "token": la credencial que dice quién es y qué
    // permisos tiene. Las apps UWP llevan en su token la marca "AppContainer".
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
        return std::nullopt;
    }
    UniqueHandle token(rawToken);

    DWORD isAppContainer = 0;
    DWORD returned = 0;
    if (!GetTokenInformation(token.Get(), TokenIsAppContainer, &isAppContainer,
                             sizeof(isAppContainer), &returned)) {
        return std::nullopt;
    }
    return isAppContainer != 0;
}

std::optional<bool> IsModuleLoaded(DWORD pid, std::wstring_view moduleName) {
    // Foto de los módulos (el .exe y todas las DLLs) cargados en el proceso.
    // Windows a veces falla con ERROR_BAD_LENGTH si el proceso está cargando
    // DLLs justo en ese momento; en ese caso se recomienda reintentar.
    UniqueHandle snapshot;
    for (int attempt = 0; attempt < 5; ++attempt) {
        snapshot = UniqueHandle(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
        if (snapshot.Valid() || GetLastError() != ERROR_BAD_LENGTH) {
            break;
        }
        Sleep(20);
    }
    if (!snapshot.Valid()) {
        return std::nullopt;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Module32FirstW(snapshot.Get(), &entry); ok;
         ok = Module32NextW(snapshot.Get(), &entry)) {
        if (_wcsicmp(entry.szModule, std::wstring(moduleName).c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::string ErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    // FormatMessage le pide a Windows el texto de un código de error.
    // Con ALLOCATE_BUFFER, Windows reserva la memoria y luego la liberamos
    // con LocalFree.
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::wstring text = length > 0 ? std::wstring(buffer, length) : L"error desconocido";
    LocalFree(buffer);

    // Quitar el salto de línea que Windows agrega al final.
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) {
        text.pop_back();
    }
    return ToUtf8(text) + " (código " + std::to_string(error) + ")";
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

}  // namespace luma::launcher
