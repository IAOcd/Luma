// =============================================================================
//  dynlight.cpp - Dynamic lights: the game-memory part (see dynlight.h)
// =============================================================================
#include "sdk/dynlight.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/timer.h"
#include "sdk/memory.h"
#include "sdk/scanner.h"

namespace luma::sdk::dynlight {

namespace {

// --- Signatures (Minecraft 1.26.63) ------------------------------------------------
// BlockSource::getBrightnessPair: movzx ebp,word [rcx+E8h] / mov [rdx],bp / mov eax,[r8+4] /
// movsx ecx,word [rcx+3Ah] / cmp eax,ecx / jl ...  (the function starts 0x22 bytes before)
constexpr const char* kBrightness = "0F B7 A9 E8 00 00 00 66 89 2A 41 8B 40 04 0F BF 49 3A 39 C8 0F 8C";
constexpr std::uintptr_t kBrightnessFromStart = 0x22;
// RenderChunkBuilder::build: mov esi,r9d / mov [rbp+?],rdx / mov r12,rcx / lea rax,[rcx+2C8h] / ...
constexpr const char* kBuild =
    "44 89 CE 48 89 95 ?? ?? ?? ?? 49 89 CC 48 8D 81 C8 02 00 00 48 89 85 ?? ?? ?? ?? 80 B9 20 04 00 00 01";
constexpr std::uintptr_t kBuildFromStart = 0x6D;
// RenderChunkCoordinator::onAreaChanged(BlockSource&, const BlockPos& min, const BlockPos& max):
// mov rdi,r9 / mov rbx,r8 / mov r8,rdx / mov rsi,rcx / ... / mov rax,[rax+90h] (getDimensionId) / ...
constexpr const char* kAreaChanged =
    "4C 89 CF 4C 89 C3 49 89 D0 48 89 CE 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 50 48 8B 02 48 8B 80 90 00 00 "
    "00 48 8D 54 24 40 4C 89 C1 FF 15";
constexpr std::uintptr_t kAreaChangedFromStart = 0x10;
// RenderChunkCoordinator::_setDirty(const BlockPos& min, const BlockPos& max, bool immediate,
// bool changesVisibility, bool canInterlock): what the game calls when you place a block. "Area
// changed" only marks pieces for "later" (it took ~2 s); this one rebuilds them right away.
// mov [rsp+2Bh],r9b / mov [rsp+48h],rcx / mov rax,[security cookie] / ... / mov r9d,[rdx] / ...
constexpr const char* kSetDirty =
    "44 88 4C 24 2B 48 89 4C 24 48 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 84 24 80 00 00 00 44 8B 0A 44 8B 7A 04 41 "
    "C1 F9 04 41 C1 FF 04";
constexpr std::uintptr_t kSetDirtyFromStart = 0x13;
constexpr std::uintptr_t kCoordinatorDimension = 0x80;  // the coordinator's dimension id
constexpr std::uintptr_t kSourceGetDimension = 0x90;    // BlockSource vtable: getDimensionId(out)
// The coordinators: LevelRenderer+0x30 is the list of an unordered_map<dimension,
// shared_ptr<RenderChunkCoordinator>>. Node: +0x0 next, +0x10 dimension, +0x18 coordinator.
constexpr std::uintptr_t kCoordinatorList = 0x30;
constexpr std::uintptr_t kNodeCoordinator = 0x18;
constexpr std::uintptr_t kAreaChangedSlot = 3;  // in the coordinator's vtable

using BrightnessFn = std::uint8_t*(__fastcall*)(void* source, std::uint8_t* out, const int* pos, std::uint32_t minBlock);
using BuildFn = void(__fastcall*)(void* self, void* geometry, bool a, int bakedLight, bool b, const void* caps);
using AreaChangedFn = void(__fastcall*)(void* coordinator, void* source, const int* min, const int* max);
using SetDirtyFn = void(__fastcall*)(void* coordinator, const int* min, const int* max, bool immediate,
                                     bool changesVisibility, bool canInterlock);
using GetDimensionFn = int*(__fastcall*)(void* source, int* out);
BrightnessFn g_brightness = nullptr;
BuildFn g_build = nullptr;
AreaChangedFn g_areaChanged = nullptr;  // only to recognize a coordinator (its vtable slot 3)
SetDirtyFn g_setDirty = nullptr;         // the game's function itself (we call it, we don't hook it)
std::atomic<bool> g_ready{false};

// --- The light ---------------------------------------------------------------------
std::atomic<int> g_x{0}, g_y{0}, g_z{0};
std::atomic<int> g_level{0};
std::atomic<bool> g_entities{true};

thread_local bool t_inBuild = false;
thread_local bool t_inRenderLevel = false;

// The BlockSource the renderer uses (captured while drawing; used to tell
// the coordinator which dimension changed). Only trusted if it was seen
// in the last frames, so a stale one (after leaving a world) is never used.
std::atomic<void*> g_source{nullptr};
std::atomic<std::uint64_t> g_sourceFrame{0};
std::uint64_t g_frame = 0;  // renderLevel calls (render-level thread only)

// --- Pending rebuild (one box that grows until it's sent) --------------------------
std::mutex g_pendingMutex;
bool g_pending = false;
int g_min[3]{}, g_max[3]{};
std::atomic<int> g_minIntervalMs{100};
std::int64_t g_lastRebuild = 0;  // render-level thread only

void AddBox(int x, int y, int z, int radius) {
    std::lock_guard lock(g_pendingMutex);
    const int lo[3] = {x - radius, y - radius, z - radius};
    const int hi[3] = {x + radius, y + radius, z + radius};
    for (int i = 0; i < 3; ++i) {
        g_min[i] = g_pending ? std::min(g_min[i], lo[i]) : lo[i];
        g_max[i] = g_pending ? std::max(g_max[i], hi[i]) : hi[i];
    }
    g_pending = true;
}

// --- Hooks -------------------------------------------------------------------------
void __fastcall BuildDetour(void* self, void* geometry, bool a, int bakedLight, bool b, const void* caps) {
    hooks::ActiveCallGuard guard;
    t_inBuild = true;
    g_build(self, geometry, a, bakedLight, b, caps);
    t_inBuild = false;
}

std::uint8_t* __fastcall BrightnessDetour(void* source, std::uint8_t* out, const int* pos, std::uint32_t minBlock) {
    hooks::ActiveCallGuard guard;
    if (t_inRenderLevel && source != nullptr) {
        g_source.store(source, std::memory_order_relaxed);
        g_sourceFrame.store(g_frame, std::memory_order_relaxed);
    }
    const int level = g_level.load(std::memory_order_relaxed);
    const bool drawing = t_inBuild || (t_inRenderLevel && g_entities.load(std::memory_order_relaxed));
    if (level > 0 && drawing && pos != nullptr) {
        // Like Minecraft's own light: one level less per block of distance
        // (counting steps along x, y and z).
        const int dist = std::abs(pos[0] - g_x.load(std::memory_order_relaxed)) +
                         std::abs(pos[1] - g_y.load(std::memory_order_relaxed)) +
                         std::abs(pos[2] - g_z.load(std::memory_order_relaxed));
        const int dynamic = level - dist;
        if (dynamic > static_cast<int>(minBlock & 0xFF)) {
            minBlock = (minBlock & ~0xFFu) | static_cast<std::uint32_t>(dynamic);
        }
    }
    return g_brightness(source, out, pos, minBlock);
}

bool HookAt(const char* pattern, std::uintptr_t fromStart, void* detour, void** original, const char* name) {
    const ScanResult match = FindPattern(pattern);
    if (match.matches != 1) {
        log::Warn("SDK: dynamic lights: {} not found ({} matches).", name, match.matches);
        return false;
    }
    void* target = reinterpret_cast<void*>(match.address - fromStart);
    return hooks::Create(target, detour, original, name) && hooks::Enable(target, name);
}

// Sends the pending box to the coordinator of the dimension being drawn.
// On the render-level thread only (the game does the same from there).
void Rebuild(std::uintptr_t levelRenderer) {
    int lo[3], hi[3];
    {
        std::lock_guard lock(g_pendingMutex);
        if (!g_pending) {
            return;
        }
        const std::int64_t now = perf::Now();
        if (perf::TicksToMs(now - g_lastRebuild) < g_minIntervalMs.load(std::memory_order_relaxed)) {
            return;  // too soon: the box keeps growing until the next time
        }
        g_lastRebuild = now;
        for (int i = 0; i < 3; ++i) {
            lo[i] = g_min[i];
            hi[i] = g_max[i];
        }
        g_pending = false;
    }
    void* source = g_source.load(std::memory_order_relaxed);
    if (source == nullptr || g_frame - g_sourceFrame.load(std::memory_order_relaxed) > 3) {
        return;
    }
    // Which dimension is the renderer drawing? (Asked to its BlockSource, like the game does.)
    std::uintptr_t sourceVtable = 0, getDimension = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(source), sourceVtable) ||
        !SafeRead(sourceVtable + kSourceGetDimension, getDimension) || getDimension < GameBase() ||
        getDimension >= GameBase() + 0x20000000) {  // must be the game's own code
        return;
    }
    int dimension = -1;
    reinterpret_cast<GetDimensionFn>(getDimension)(source, &dimension);
    std::uintptr_t head = 0;
    if (!SafeRead(levelRenderer + kCoordinatorList, head) || !LooksLikePointer(head)) {
        return;
    }
    std::uintptr_t node = 0;
    SafeRead(head, node);
    for (int i = 0; i < 8 && node != head && LooksLikePointer(node); ++i) {
        std::uintptr_t coordinator = 0, vtable = 0, slot = 0;
        // Only if it really is a coordinator: its vtable's "area changed" is the game's function.
        if (SafeRead(node + kNodeCoordinator, coordinator) && LooksLikePointer(coordinator) &&
            SafeRead(coordinator, vtable) && SafeRead(vtable + kAreaChangedSlot * 8, slot) &&
            slot == reinterpret_cast<std::uintptr_t>(g_areaChanged)) {
            int coordinatorDimension = -2;
            if (SafeRead(coordinator + kCoordinatorDimension, coordinatorDimension) &&
                coordinatorDimension == dimension) {
                g_setDirty(reinterpret_cast<void*>(coordinator), lo, hi, /*immediate=*/true,
                           /*changesVisibility=*/false, /*canInterlock=*/true);
            }
        }
        if (!SafeRead(node, node)) {
            break;
        }
    }
}

}  // namespace

bool Init() {
    const ScanResult area = FindPattern(kAreaChanged);
    if (area.matches != 1) {
        log::Warn("SDK: dynamic lights: RenderChunkCoordinator::onAreaChanged not found ({} matches).", area.matches);
        return false;
    }
    g_areaChanged = reinterpret_cast<AreaChangedFn>(area.address - kAreaChangedFromStart);
    const ScanResult dirty = FindPattern(kSetDirty);
    if (dirty.matches != 1) {
        log::Warn("SDK: dynamic lights: RenderChunkCoordinator::_setDirty not found ({} matches).", dirty.matches);
        return false;
    }
    g_setDirty = reinterpret_cast<SetDirtyFn>(dirty.address - kSetDirtyFromStart);
    if (!HookAt(kBrightness, kBrightnessFromStart, reinterpret_cast<void*>(&BrightnessDetour),
                reinterpret_cast<void**>(&g_brightness), "game: light of a block (getBrightnessPair)") ||
        !HookAt(kBuild, kBuildFromStart, reinterpret_cast<void*>(&BuildDetour), reinterpret_cast<void**>(&g_build),
                "game: build a piece of the world (RenderChunkBuilder::build)")) {
        return false;
    }
    g_ready.store(true);
    log::Info("SDK: dynamic lights ready (onAreaChanged at game+0x{:X}).",
              reinterpret_cast<std::uintptr_t>(g_areaChanged) - GameBase());
    return true;
}

bool Available() {
    return g_ready.load(std::memory_order_relaxed);
}

void SetLight(int x, int y, int z, int level) {
    level = std::clamp(level, 0, 15);
    const int oldLevel = g_level.load();
    const int ox = g_x.load(), oy = g_y.load(), oz = g_z.load();
    if (level == oldLevel && (level == 0 || (x == ox && y == oy && z == oz))) {
        return;  // nothing changed: nothing to rebuild
    }
    // Rebuild where the light was (to remove it) and where it is now.
    if (oldLevel > 0) {
        AddBox(ox, oy, oz, oldLevel);
    }
    if (level > 0) {
        AddBox(x, y, z, level);
    }
    g_x.store(x);
    g_y.store(y);
    g_z.store(z);
    g_level.store(level);
}

void SetLightEntities(bool on) {
    g_entities.store(on);
}

void SetMaxRebuildsPerSecond(int perSecond) {
    g_minIntervalMs.store(1000 / std::clamp(perSecond, 1, 30));
}

void EnterRenderLevel(std::uintptr_t levelRenderer) {
    if (!g_ready.load(std::memory_order_relaxed)) {
        return;
    }
    ++g_frame;
    Rebuild(levelRenderer);
    t_inRenderLevel = true;
}

void LeaveRenderLevel() {
    t_inRenderLevel = false;
}

}  // namespace luma::sdk::dynlight
