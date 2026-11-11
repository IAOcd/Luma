// =============================================================================
//  system_info.h - Facts about the PC and the game (for the debug screen, F3)
// =============================================================================
//  What doesn't change (CPU, graphics card, Windows and Minecraft versions)
//  is read once and kept. Memory is read at most once a second. Everything
//  comes from Windows itself; nothing is sent anywhere.
// =============================================================================
#pragma once

#include <string>

namespace luma::perf::sysinfo {

const std::string& CpuName();        // "AMD Ryzen 5 5600G with Radeon Graphics"
const std::string& GpuName();        // the card the game draws with ("AMD Radeon(TM) Graphics")
const std::string& WindowsName();    // "Windows 11 (26200)"
const std::string& GameVersion();    // "1.26.63" (from Minecraft.Windows.exe)
int CpuThreads();

struct Memory {
    double gameMb = 0.0;   // what Minecraft uses (its private memory)
    double usedMb = 0.0;   // the whole PC, in use
    double totalMb = 0.0;  // the whole PC
};
const Memory& MemoryNow();  // refreshed at most once a second

}  // namespace luma::perf::sysinfo
