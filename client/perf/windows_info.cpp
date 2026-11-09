// =============================================================================
//  windows_info.cpp - Implementación de la lectura de opciones de Windows
// =============================================================================
#include "perf/windows_info.h"

#include <windows.h>
#include <powrprof.h>

#include <string>

#include "core/paths.h"

namespace luma::perf::windows {

namespace {

bool ReadDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD& out) {
    DWORD size = sizeof(out);
    return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

bool ReadString(HKEY root, const wchar_t* path, const wchar_t* name, std::wstring& out) {
    wchar_t buffer[512];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return false;
    }
    out = buffer;
    return true;
}

std::string Utf8(const std::wstring& text) {
    return paths::ToUtf8(std::filesystem::path(text));
}

}  // namespace

Info Read() {
    Info info;

    // Modo de juego: si el valor no existe, en Windows 10/11 está ACTIVADO.
    DWORD value = 0;
    if (ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", value)) {
        info.gameMode = value != 0 ? Status::On : Status::Off;
    } else {
        info.gameMode = Status::On;
    }

    // HAGS: 2 = activada, 1 = desactivada. Si no existe, la GPU o el driver no la soportan.
    if (ReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", value)) {
        info.gpuScheduling = value == 2 ? Status::On : Status::Off;
    }

    // Plan de energía activo y su nombre.
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme != nullptr) {
        wchar_t name[256] = L"";
        DWORD size = sizeof(name);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR*>(name), &size) ==
            ERROR_SUCCESS) {
            info.powerPlan = Utf8(name);
        }
        // GUID del plan "Alto rendimiento" (GUID_MIN_POWER_SAVINGS) o "Máximo rendimiento".
        static const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
        info.powerPlanIsHighPerformance = IsEqualGUID(*scheme, GUID_MIN_POWER_SAVINGS) || IsEqualGUID(*scheme, kUltimate);
        LocalFree(scheme);
    }

    // Preferencias de DirectX de este usuario (Configuración > Pantalla > Gráficos).
    const wchar_t* prefsPath = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring text;
    info.gpuPreference = "Lo decide Windows";
    if (ReadString(HKEY_CURRENT_USER, prefsPath, exe, text)) {
        if (text.find(L"GpuPreference=2") != std::wstring::npos) {
            info.gpuPreference = "Alto rendimiento";
        } else if (text.find(L"GpuPreference=1") != std::wstring::npos) {
            info.gpuPreference = "Ahorro de energía";
        }
    }
    if (ReadString(HKEY_CURRENT_USER, prefsPath, L"DirectXUserGlobalSettings", text)) {
        if (text.find(L"SwapEffectUpgradeEnable=1") != std::wstring::npos) {
            info.windowedOptimizations = Status::On;
        } else if (text.find(L"SwapEffectUpgradeEnable=0") != std::wstring::npos) {
            info.windowedOptimizations = Status::Off;
        }
    }
    return info;
}

}  // namespace luma::perf::windows
