// =============================================================================
//  paths.cpp - Data folders implementation
// =============================================================================
//  A ".cpp" holds the real code of the functions the ".h" declares.
// =============================================================================
#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>  // SHGetKnownFolderPath: pedirle a Windows carpetas conocidas

namespace luma::paths {

std::filesystem::path DataDir() {
    // We ask Windows for the path of the "Roaming AppData" folder.
    // Windows allocates memory for the text and gives us a pointer to it;
    // afterwards we have to give it back with CoTaskMemFree.
    PWSTR raw = nullptr;
    HRESULT result = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);

    std::filesystem::path dir;
    if (SUCCEEDED(result) && raw != nullptr) {
        // The "/" operator joins parts of a path: "C:\...\Roaming" / "Luma".
        dir = std::filesystem::path(raw) / L"Luma";
    }

    // Always called, even if it failed (it's safe with nullptr).
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
    const std::wstring& wide = path.native();  // On Windows, paths are "wide" text (UTF-16).
    if (wide.empty()) {
        return {};
    }
    // First call: ask how many bytes are needed. Second: convert.
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

}  // namespace luma::paths
