// =============================================================================
//  paths.cpp - Implementación de las carpetas de datos
// =============================================================================
//  Un ".cpp" contiene el código real de las funciones que el ".h" declara.
// =============================================================================
#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>  // SHGetKnownFolderPath: pedirle a Windows carpetas conocidas

namespace luma::paths {

std::filesystem::path DataDir() {
    // Le pedimos a Windows la ruta de la carpeta "Roaming AppData".
    // Windows reserva memoria para el texto y nos devuelve un puntero a ella;
    // después tenemos que devolverla con CoTaskMemFree.
    PWSTR raw = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);

    std::filesystem::path dir;
    if (SUCCEEDED(result) && raw != nullptr) {
        // El operador "/" une partes de una ruta: "C:\...\Roaming" / "Luma".
        dir = std::filesystem::path(raw) / L"Luma";
    }

    // Se llama siempre, aunque haya fallado (es seguro con nullptr).
    CoTaskMemFree(raw);
    return dir;
}

std::filesystem::path LogsDir() {
    std::filesystem::path data = DataDir();
    if (data.empty()) {
        return {};
    }
    return data / L"logs";
}

std::string ToUtf8(const std::filesystem::path& path) {
    const std::wstring& wide = path.native();  // En Windows, las rutas son texto "ancho" (UTF-16).
    if (wide.empty()) {
        return {};
    }
    // Primera llamada: preguntar cuántos bytes hacen falta. Segunda: convertir.
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

}  // namespace luma::paths
