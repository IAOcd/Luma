// =============================================================================
//  system_info.cpp - Facts about the PC and the game (see system_info.h)
// =============================================================================
#include "perf/system_info.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <vector>

#include "perf/timer.h"
#include "render/gpu.h"

namespace luma::perf::sysinfo {

namespace {

std::string Narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

}  // namespace

const std::string& CpuName() {
    static const std::string name = [] {
        wchar_t buffer[128] = L"";
        DWORD size = sizeof(buffer);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                         RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
            return Trim(Narrow(buffer));
        }
        return std::string("?");
    }();
    return name;
}

int CpuThreads() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return static_cast<int>(info.dwNumberOfProcessors);
}

const std::string& GpuName() {
    static std::string name;
    if (name.empty()) {
        name = render::gpu::AdapterName();  // empty until the overlay has the game's device
    }
    return name;
}

const std::string& WindowsName() {
    static const std::string name = [] {
        // RtlGetVersion tells the truth (GetVersionEx lies to apps without a manifest).
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW info{sizeof(info)};
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
            fn(&info);
        }
        char text[64];
        std::snprintf(text, sizeof(text), "Windows %s (%lu)", info.dwBuildNumber >= 22000 ? "11" : "10", info.dwBuildNumber);
        return std::string(text);
    }();
    return name;
}

const std::string& GameVersion() {
    static const std::string version = [] {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return std::string("?");
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
        if (size == 0) return std::string("?");
        std::vector<std::uint8_t> data(size);
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!GetFileVersionInfoW(path, 0, size, data.data()) ||
            !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || info == nullptr) {
            return std::string("?");
        }
        // 1.26.6301.0 -> "1.26.63" (the third number is "63" + "01")
        const unsigned major = HIWORD(info->dwProductVersionMS), minor = LOWORD(info->dwProductVersionMS);
        const unsigned build = HIWORD(info->dwProductVersionLS);
        char text[32];
        std::snprintf(text, sizeof(text), "%u.%u.%u", major, minor, build >= 100 ? build / 100 : build);
        return std::string(text);
    }();
    return version;
}

const Memory& MemoryNow() {
    static Memory memory;
    static std::int64_t next = 0;
    const std::int64_t now = Now();
    if (now >= next) {
        next = now + static_cast<std::int64_t>(g_ticksPerSecond);
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            memory.gameMb = static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
        }
        MEMORYSTATUSEX status{sizeof(status)};
        if (GlobalMemoryStatusEx(&status)) {
            memory.totalMb = static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0);
            memory.usedMb = static_cast<double>(status.ullTotalPhys - status.ullAvailPhys) / (1024.0 * 1024.0);
        }
    }
    return memory;
}

}  // namespace luma::perf::sysinfo
