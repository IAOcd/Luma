// =============================================================================
//  windows_info.h - Reading (ONLY reading) Windows settings useful for gaming
// =============================================================================
//  For the menu's help section: Luma shows you how these options are set
//  and how to change them, but NEVER changes them by itself.
//
//  They're all read from the Windows registry or with official functions:
//    - Game Mode:       HKCU\Software\Microsoft\GameBar, AutoGameModeEnabled
//    - Hardware-accelerated GPU scheduling ("HAGS"):
//                       HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers, HwSchMode (2 = yes)
//    - Power plan:      PowerGetActiveScheme + its name
//    - Preferred GPU for Minecraft:
//                       HKCU\Software\Microsoft\DirectX\UserGpuPreferences, value = path of the .exe
//    - Optimizations for windowed games: the same place, "DirectXUserGlobalSettings"
// =============================================================================
#pragma once

#include <string>

namespace luma::perf::windows {

enum class Status { On, Off, Unknown };

struct Info {
    Status gameMode = Status::Unknown;
    Status gpuScheduling = Status::Unknown;
    Status windowedOptimizations = Status::Unknown;
    std::string powerPlan;          // name of the active plan ("Balanced"...)
    bool powerPlanIsHighPerformance = false;
    std::string gpuPreference;      // "High performance", "Power saving", "Windows decides"
};

// Reads everything (a few milliseconds: call it when opening the page, not per frame).
Info Read();

}  // namespace luma::perf::windows
