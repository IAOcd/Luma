// =============================================================================
//  network.cpp - Hook del ping (ver network.h)
// =============================================================================
#include "sdk/network.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/timer.h"
#include "sdk/memory.h"
#include "sdk/player.h"
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

// --- Leer el ping de la conexión (lo que hace el juego para su propio menú) -----------
//  El hook de arriba solo se entera si el JUEGO pide el ping, y en muchos
//  servidores nunca lo pide: por eso el módulo no aparecía. Ahora lo pedimos
//  nosotros una vez por segundo, siguiendo la cadena (1.26.6x; la idea viene
//  del client open source Latite, el código es propio):
//
//    Player +0x820 -> PacketSender +0x20 -> NetworkSystem
//    NetworkSystem +0xE8 -> std::vector de conexiones (unique_ptr)
//    Conexión +0x118 -> NetworkPeer; su función virtual n.º 2 llena
//    NetworkStatus { int32 nivel; int64 ping actual; int64 ping promedio; ... }
//
//  Antes de llamar a la función, comprobamos que esté DENTRO del código del
//  juego (si no, la cadena no es la que creemos y no se llama nada).
constexpr std::uintptr_t kPlayerPacketSender = 0x820;
constexpr std::uintptr_t kSenderNetworkSystem = 0x20;
constexpr std::uintptr_t kSystemConnections = 0xE8;
constexpr std::uintptr_t kConnectionPeer = 0x118;
constexpr std::uintptr_t kConnectionSkip = 0x199;

struct NetworkStatus {
    std::int32_t level;
    std::int64_t currentPing;
    std::int64_t averagePing;
    char pad[0x40];  // (más espacio del que usa el juego, por las dudas)
};
using GetStatusFn = void(__fastcall*)(void* peer, NetworkStatus* out);

bool InsideGame(std::uintptr_t address) {
    const std::uintptr_t base = GameBase();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
}

bool CallStatus(GetStatusFn fn, void* peer, NetworkStatus& status) {
    __try {
        fn(peer, &status);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// El ping de la conexión con el servidor, o -1.
int ReadConnectionPing() {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t sender = 0;
    std::uintptr_t system = 0;
    std::uintptr_t list[2]{};
    if (player == 0 || !SafeRead(player + kPlayerPacketSender, sender) || !LooksLikePointer(sender) ||
        !SafeRead(sender + kSenderNetworkSystem, system) || !LooksLikePointer(system) ||
        !SafeRead(system + kSystemConnections, list) || list[1] < list[0] || (list[1] - list[0]) / 8 > 16) {
        return -1;
    }
    for (std::uintptr_t it = list[0]; it < list[1]; it += 8) {
        std::uintptr_t connection = 0;
        std::uintptr_t peer = 0;
        std::uint8_t skip = 0;
        std::uintptr_t vtable = 0;
        std::uintptr_t fn = 0;
        if (!SafeRead(it, connection) || !LooksLikePointer(connection) || !SafeRead(connection + kConnectionSkip, skip) ||
            skip != 0 || !SafeRead(connection + kConnectionPeer, peer) || !LooksLikePointer(peer) ||
            !SafeRead(peer, vtable) || !LooksLikePointer(vtable) || !SafeRead(vtable + 2 * 8, fn) || !InsideGame(fn)) {
            continue;
        }
        NetworkStatus status{};
        if (CallStatus(reinterpret_cast<GetStatusFn>(fn), reinterpret_cast<void*>(peer), status) &&
            status.currentPing >= 0 && status.currentPing < 100000) {
            return static_cast<int>(status.currentPing);
        }
    }
    return -1;
}

std::atomic<int> g_connectionPing{-1};
std::atomic<bool> g_loggedConnection{false};

}  // namespace

void Poll() {
    // Una vez por segundo (lo llama el hilo principal de Luma cada ~50 ms).
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!g_active.load(std::memory_order_relaxed) || now - last < 1000) {
        return;
    }
    last = now;
    const int ping = ReadConnectionPing();
    g_connectionPing.store(ping, std::memory_order_relaxed);
    if (ping > 0 && !g_loggedConnection.exchange(true)) {
        log::Info("SDK: ping leído de la conexión: {} ms.", ping);
    }
}

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
    // En un mundo local la conexión es "contigo mismo" y el ping da 0.
    if (g_connectionPing.load(std::memory_order_relaxed) > 0) {
        return true;
    }
    const std::int64_t last = g_lastValid.load(std::memory_order_relaxed);
    return last != 0 && perf::TicksToMs(perf::Now() - last) < kServerTimeoutMs;
}

int PingMs() {
    const int fromConnection = g_connectionPing.load(std::memory_order_relaxed);
    if (fromConnection > 0) {
        return fromConnection;
    }
    return OnServer() ? g_ping.load(std::memory_order_relaxed) : -1;
}

void BeginUnload() {
    g_active.store(false);
}

}  // namespace luma::sdk::network
