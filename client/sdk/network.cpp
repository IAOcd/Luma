// =============================================================================
//  network.cpp - Hook del ping (ver network.h)
// =============================================================================
#include "sdk/network.h"

#include <atomic>
#include <cstdint>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/timer.h"
#include "sdk/scanner.h"

namespace luma::sdk::network {

namespace {

// Inicio de RakPeer::GetAveragePing en 1.26.6x. Es largo porque el comienzo
// se parece al de otras funciones vecinas de RakNet (con solo el principio
// aparecían 4 coincidencias): el final lo hace único.
constexpr const char* kGetAveragePing =
    "48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 ?? ?? ?? ?? 4C 8B 02 4C 3B 05 ?? ?? ?? ?? "
    "0F 85 ?? ?? ?? ?? 0F B7 42 ?? F2 0F 10 82 ?? ?? ?? ?? 66 89 44 24 ?? 0F 10 4A ?? 0F 10 52 ?? 0F 10 5A ?? "
    "0F 10 62 ?? 0F 11 4C 24 ?? 0F 11 54 24 ?? 0F 11 5C 24 ?? 0F 11 64 24 ?? 0F 10 4A ?? 0F 11 4C 24 ?? "
    "0F 10 4A ?? 0F 11 4C 24 ?? 0F 10 4A ?? 0F 11 8C 24 ?? ?? ?? ?? 0F 10 8A ?? ?? ?? ?? 0F 11 8C 24 ?? ?? ?? ?? "
    "F3 0F 11 84 24 ?? ?? ?? ?? 48 8D 54 24 ?? 45 31 C0 45 31 C9 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 0F B7 88";

constexpr double kServerTimeoutMs = 5000.0;

using GetAveragePingFn = int(__fastcall*)(void* peer, void* guidOrAddress);
GetAveragePingFn g_original = nullptr;

std::atomic<bool> g_active{false};
std::atomic<int> g_ping{-1};
std::atomic<std::int64_t> g_lastValid{0};
std::atomic<bool> g_loggedServer{false};

int __fastcall GetAveragePingDetour(void* peer, void* guidOrAddress) {
    hooks::ActiveCallGuard guard;
    const int ping = g_original(peer, guidOrAddress);
    if (g_active.load(std::memory_order_relaxed) && ping >= 0 && ping < 100000) {
        g_ping.store(ping, std::memory_order_relaxed);
        g_lastValid.store(perf::Now(), std::memory_order_relaxed);
        if (!g_loggedServer.exchange(true)) {
            log::Info("SDK: conectado a un servidor (ping {} ms).", ping);
        }
    }
    return ping;
}

}  // namespace

bool Init() {
    const ScanResult match = FindPattern(kGetAveragePing);
    if (match.matches != 1) {
        log::Warn("SDK: firma del ping no encontrada ({} coincidencias). Ping y detección de servidor desactivados.",
                  match.matches);
        return false;
    }
    void* target = reinterpret_cast<void*>(match.address);
    if (!hooks::Create(target, reinterpret_cast<void*>(&GetAveragePingDetour), reinterpret_cast<void**>(&g_original),
                       "juego: ping promedio (RakNet)") ||
        !hooks::Enable(target, "juego: ping promedio (RakNet)")) {
        return false;
    }
    g_active.store(true);
    log::Info("SDK: ping promedio (RakNet) en juego+0x{:X}.", match.address - GameBase());
    return true;
}

bool Available() {
    return g_active.load(std::memory_order_relaxed);
}

bool OnServer() {
    const std::int64_t last = g_lastValid.load(std::memory_order_relaxed);
    return last != 0 && perf::TicksToMs(perf::Now() - last) < kServerTimeoutMs;
}

int PingMs() {
    return OnServer() ? g_ping.load(std::memory_order_relaxed) : -1;
}

void BeginUnload() {
    g_active.store(false);
}

}  // namespace luma::sdk::network
