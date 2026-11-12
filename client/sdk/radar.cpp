// =============================================================================
//  radar.cpp - The entities around you (see radar.h) - LOCAL WORLDS ONLY
// =============================================================================
#include "sdk/radar.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include "sdk/entity.h"
#include "sdk/memory.h"
#include "sdk/network.h"
#include "sdk/offsets.h"
#include "sdk/player.h"

namespace luma::sdk::radar {

namespace {

// The same components the hitboxes use (sdk/draw3d.cpp, 1.26.63).
constexpr std::uint32_t kAabbHash = 0xBAC1B3CF;       // AABBShapeComponent: min, max, size
constexpr std::uintptr_t kAabbSize = 0x20;
constexpr std::uint32_t kActorTypeHash = 0x4F6BA419;  // ActorTypeComponent: the type number
constexpr std::uint32_t kVersionShift = 18;
constexpr std::uint32_t kTombstoneVersion = 0xFFFFFFFFu >> kVersionShift;
constexpr std::size_t kMax = 512;

std::atomic<bool> g_wanted{false};
std::mutex g_mutex;
std::vector<Entity> g_list;  // the latest (copied out by Copy)
std::vector<Entity> g_work;  // being filled (game thread only)
int g_frame = 0;

void Collect() {
    g_work.clear();
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t registry = 0;
    std::uint32_t ownId = 0;
    if (player == 0 || !SafeRead(player + offsets::kActorEnttRegistry, registry) || !LooksLikePointer(registry) ||
        !SafeRead(player + offsets::kActorEntityId, ownId)) {
        return;
    }
    const std::uintptr_t boxes = entity::Storage(registry, kAabbHash);
    const std::uintptr_t types = entity::Storage(registry, kActorTypeHash);
    std::uintptr_t entities[2]{};
    std::uintptr_t pages = 0;
    if (boxes == 0 || types == 0 || !SafeRead(boxes + 0x20, entities) || !SafeRead(boxes + offsets::kStoragePayload, pages) ||
        entities[1] < entities[0]) {
        return;
    }
    float own[3]{};
    std::uintptr_t ownAabb = 0;
    const bool haveOwn = SafeRead(player + offsets::kActorAabb, ownAabb) && LooksLikeDataPointer(ownAabb) && SafeRead(ownAabb, own);
    const std::size_t count = std::min<std::size_t>((entities[1] - entities[0]) / 4, 8192);
    for (std::size_t i = 0; i < count && g_work.size() < kMax; ++i) {
        std::uint32_t id = 0;
        if (!SafeRead(entities[0] + i * 4, id) || (id & offsets::kEntityMask) == offsets::kEntityMask || (id >> kVersionShift) == kTombstoneVersion) {
            continue;  // an empty place or what's left of a removed entity
        }
        if ((id & offsets::kEntityMask) == (ownId & offsets::kEntityMask)) continue;  // you
        std::uintptr_t page = 0;
        if (!SafeRead(pages + (i / offsets::kPayloadPage) * 8, page) || !LooksLikePointer(page)) continue;
        const std::uintptr_t comp = page + (i % offsets::kPayloadPage) * kAabbSize;
        if (entity::ComponentOf(boxes, id, kAabbSize) != comp) continue;  // an old place
        const std::uintptr_t typeComp = entity::ComponentOf(types, id, 4);
        std::uint32_t type = 0;
        if (typeComp == 0 || !SafeRead(typeComp, type) || type == 0) continue;
        float box[8]{};
        if (!SafeRead(comp, box) || !std::isfinite(box[0]) || !std::isfinite(box[5]) || box[6] <= 0.0f || box[6] > 8.0f) continue;
        Entity e;
        e.x = (box[0] + box[3]) * 0.5f;
        e.y = box[1];
        e.z = (box[2] + box[5]) * 0.5f;
        e.type = type;
        e.kind = KindOf(type);
        // In a local world the game keeps a second copy of YOU (its inner
        // server's): a player right where you are is skipped.
        if (e.kind == Kind::Player && haveOwn && std::fabs(box[0] - own[0]) < 0.6f && std::fabs(box[2] - own[2]) < 0.6f &&
            std::fabs(box[1] - own[1]) < 2.5f) {
            continue;
        }
        g_work.push_back(e);
    }
}

}  // namespace

Kind KindOf(std::uint32_t type) {
    if (type == 319) return Kind::Player;
    if (type == 64) return Kind::Item;
    if ((type & 0x1000000) != 0) return Kind::Villager;
    if ((type & 0x800) != 0) return Kind::Hostile;
    if ((type & 0x2000) != 0) return Kind::Water;
    if ((type & 0x1000) != 0) return Kind::Animal;
    if ((type & 0x8000) != 0) return Kind::Ambient;
    return Kind::Other;  // golems, armor stands, boats, minecarts, arrows...
}

void Want(bool want) {
    g_wanted.store(want, std::memory_order_relaxed);
}

void OnFrame() {
    if (!g_wanted.load(std::memory_order_relaxed) || ++g_frame % 6 != 0) return;
    if (sdk::network::OnServer()) {
        // A server: nothing, ever (Luma's rule: no multiplayer advantage).
        std::lock_guard lock(g_mutex);
        g_list.clear();
        return;
    }
    Collect();
    std::lock_guard lock(g_mutex);
    g_list.swap(g_work);
}

void Copy(std::vector<Entity>& out) {
    std::lock_guard lock(g_mutex);
    if (sdk::network::OnServer()) {
        out.clear();
        return;
    }
    out = g_list;
}

}  // namespace luma::sdk::radar
