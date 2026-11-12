// =============================================================================
//  map_store.cpp - What you explored (see map_store.h)
// =============================================================================
#include "modules/world/map_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "core/logger.h"
#include "core/paths.h"

namespace luma::modules::world::mapstore {

namespace {

namespace fs = std::filesystem;

struct Region {
    std::vector<std::uint32_t> px;  // kRegion * kRegion, row by row
    bool dirty = false;              // changed since it was saved
    double changedAt = 0.0;
    std::uint64_t used = 0;          // the last "use" (for forgetting the oldest)
};

constexpr std::size_t kMaxRegions = 256;  // (256 KB each: 64 MB at most; only explored regions count)
constexpr char kMagic[4] = {'L', 'M', 'R', '1'};

std::unordered_map<std::uint64_t, std::unique_ptr<Region>> g_regions;
std::unordered_map<std::uint64_t, bool> g_missing;  // regions with no file (no need to look again)
std::string g_world;
int g_dimension = -1;
fs::path g_dir;
std::uint32_t g_version = 0;
std::uint64_t g_use = 0;
double g_now = 0.0;
// The last region asked for (most calls ask for the same one).
std::uint64_t g_lastKey = ~0ull;
Region* g_last = nullptr;

std::uint64_t KeyOf(int rx, int rz) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rx)) << 32) | static_cast<std::uint32_t>(rz);
}

fs::path FileOf(int rx, int rz) {
    char name[48];
    std::snprintf(name, sizeof(name), "r.%d.%d.bin", rx, rz);
    return g_dir / name;
}

bool Save(int rx, int rz, Region& r) {
    r.dirty = false;
    if (g_dir.empty()) return false;
    std::vector<std::uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + 4);
    const std::size_t n = r.px.size();
    for (std::size_t i = 0; i < n;) {
        const std::uint32_t v = r.px[i];
        std::size_t run = 1;
        while (i + run < n && r.px[i + run] == v && run < 65535) ++run;
        const std::uint16_t c = static_cast<std::uint16_t>(run);
        out.push_back(static_cast<std::uint8_t>(c & 0xFF));
        out.push_back(static_cast<std::uint8_t>(c >> 8));
        for (int b = 0; b < 4; ++b) out.push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF));
        i += run;
    }
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    const fs::path path = FileOf(rx, rz);
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    }
    fs::rename(temp, path, ec);
    if (ec) {
        log::Warn("World map: couldn't save {} ({})", paths::ToUtf8(path), ec.message());
        return false;
    }
    g_missing.erase(KeyOf(rx, rz));  // (it has a file now)
    return true;
}

bool LoadInto(int rx, int rz, Region& r) {
    std::ifstream file(FileOf(rx, rz), std::ios::binary);
    if (!file) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 4 || std::memcmp(data.data(), kMagic, 4) != 0) return false;
    std::size_t at = 0;
    for (std::size_t p = 4; p + 6 <= data.size() && at < r.px.size(); p += 6) {
        const auto b = reinterpret_cast<const std::uint8_t*>(data.data() + p);
        const std::size_t run = static_cast<std::size_t>(b[0] | (b[1] << 8));
        const std::uint32_t v = static_cast<std::uint32_t>(b[2]) | (static_cast<std::uint32_t>(b[3]) << 8) |
                                (static_cast<std::uint32_t>(b[4]) << 16) | (static_cast<std::uint32_t>(b[5]) << 24);
        const std::size_t end = std::min(r.px.size(), at + run);
        std::fill(r.px.begin() + static_cast<std::ptrdiff_t>(at), r.px.begin() + static_cast<std::ptrdiff_t>(end), v);
        at = end;
    }
    return true;
}

// Forget the least used region (saving it first if it changed).
void MakeRoom() {
    while (g_regions.size() >= kMaxRegions) {
        auto oldest = g_regions.end();
        for (auto it = g_regions.begin(); it != g_regions.end(); ++it) {
            if (oldest == g_regions.end() || it->second->used < oldest->second->used) oldest = it;
        }
        if (oldest == g_regions.end()) return;
        const int rx = static_cast<int>(static_cast<std::int32_t>(oldest->first >> 32));
        const int rz = static_cast<int>(static_cast<std::int32_t>(oldest->first & 0xFFFFFFFFu));
        if (oldest->second->dirty) Save(rx, rz, *oldest->second);
        if (g_last == oldest->second.get()) {
            g_last = nullptr;
            g_lastKey = ~0ull;
        }
        g_regions.erase(oldest);
    }
}

// The region (rx, rz): from memory, from disk ("load"), or new and empty ("create").
Region* Get(int rx, int rz, bool load, bool create) {
    const std::uint64_t key = KeyOf(rx, rz);
    if (key == g_lastKey && g_last != nullptr) {
        g_last->used = ++g_use;
        return g_last;
    }
    auto it = g_regions.find(key);
    if (it == g_regions.end()) {
        const bool missing = g_missing.count(key) != 0;
        if (!create && (!load || missing)) return nullptr;
        if (g_dir.empty()) return nullptr;
        MakeRoom();
        auto r = std::make_unique<Region>();
        r->px.assign(static_cast<std::size_t>(kRegion) * kRegion, 0);
        if (!missing && !LoadInto(rx, rz, *r)) {
            g_missing[key] = true;
            if (!create) return nullptr;
        }
        it = g_regions.emplace(key, std::move(r)).first;
    }
    it->second->used = ++g_use;
    g_lastKey = key;
    g_last = it->second.get();
    return g_last;
}

}  // namespace

void SetWorld(const std::string& storageName, int dimension) {
    if (storageName == g_world && dimension == g_dimension) return;
    Flush();
    g_regions.clear();
    g_missing.clear();
    g_last = nullptr;
    g_lastKey = ~0ull;
    g_world = storageName;
    g_dimension = dimension;
    g_dir.clear();
    if (!storageName.empty() && dimension >= 0) {
        const fs::path base = paths::DataDir();
        if (!base.empty()) g_dir = base / L"worldmap" / fs::path(storageName) / ("dim" + std::to_string(dimension));
    }
    ++g_version;
}

void Merge(const std::uint32_t* pixels, int side, int x0, int z0) {
    if (g_dir.empty() || pixels == nullptr) return;
    bool changed = false;
    for (int r = 0; r < side; ++r) {
        const int z = z0 + r;
        const int rz = RegionOf(z);
        const int lz = z - rz * kRegion;
        for (int c = 0; c < side;) {
            const int x = x0 + c;
            const int rx = RegionOf(x);
            // The part of this row inside region (rx, rz).
            const int end = std::min(side, (rx + 1) * kRegion - x0);
            Region* reg = Get(rx, rz, true, true);
            if (reg != nullptr) {
                std::uint32_t* dst = reg->px.data() + static_cast<std::size_t>(lz) * kRegion + (x - rx * kRegion);
                for (int k = c; k < end; ++k, ++dst) {
                    const std::uint32_t v = pixels[r * side + k];
                    if (v != 0 && *dst != v) {
                        *dst = v;
                        if (!reg->dirty) reg->changedAt = g_now;
                        reg->dirty = true;
                        changed = true;
                    }
                }
            }
            c = end;
        }
    }
    if (changed) ++g_version;
}

void MergeRing(const std::uint32_t* ring, int side, int x0, int z0, int x1, int z1) {
    if (g_dir.empty() || ring == nullptr) return;
    const int mask = side - 1;
    bool changed = false;
    for (int z = z0; z < z1; ++z) {
        const int rz = RegionOf(z);
        const int lz = z - rz * kRegion;
        const std::uint32_t* src = ring + static_cast<std::size_t>(z & mask) * side;
        for (int x = x0; x < x1;) {
            const int rx = RegionOf(x);
            const int end = std::min(x1, (rx + 1) * kRegion);
            Region* reg = Get(rx, rz, true, true);
            if (reg != nullptr) {
                std::uint32_t* dst = reg->px.data() + static_cast<std::size_t>(lz) * kRegion;
                for (int k = x; k < end; ++k) {
                    const std::uint32_t v = src[k & mask];
                    std::uint32_t& d = dst[k - rx * kRegion];
                    if (v != 0 && d != v) {
                        d = v;
                        if (!reg->dirty) reg->changedAt = g_now;
                        reg->dirty = true;
                        changed = true;
                    }
                }
            }
            x = end;
        }
    }
    if (changed) ++g_version;
}

std::uint32_t At(int x, int z, bool load) {
    const int rx = RegionOf(x), rz = RegionOf(z);
    Region* reg = Get(rx, rz, load, false);
    if (reg == nullptr) return 0;
    return reg->px[static_cast<std::size_t>(z - rz * kRegion) * kRegion + static_cast<std::size_t>(x - rx * kRegion)];
}

const std::uint32_t* Row(int rx, int rz, int z) {
    Region* reg = Get(rx, rz, false, false);
    if (reg == nullptr) return nullptr;
    return reg->px.data() + static_cast<std::size_t>(z - rz * kRegion) * kRegion;
}

bool Prefetch(int x0, int z0, int x1, int z1, int maxLoads) {
    if (g_dir.empty()) return true;
    // The middle first (what you look at), for at most 3 ms.
    const int rx0 = RegionOf(x0), rx1 = RegionOf(x1), rz0 = RegionOf(z0), rz1 = RegionOf(z1);
    const int mx = (rx0 + rx1) / 2, mz = (rz0 + rz1) / 2;
    const auto start = std::chrono::steady_clock::now();
    bool all = true;
    bool loaded = false;
    const int reach = std::max(std::max(mx - rx0, rx1 - mx), std::max(mz - rz0, rz1 - mz));
    for (int ring = 0; ring <= reach; ++ring) {
        for (int rz = mz - ring; rz <= mz + ring; ++rz) {
            for (int rx = mx - ring; rx <= mx + ring; ++rx) {
                if (std::max(std::abs(rx - mx), std::abs(rz - mz)) != ring) continue;  // (only this ring)
                if (rx < rx0 || rx > rx1 || rz < rz0 || rz > rz1) continue;
                const std::uint64_t key = KeyOf(rx, rz);
                if (g_regions.count(key) != 0 || g_missing.count(key) != 0) continue;
                if (maxLoads-- <= 0 || std::chrono::steady_clock::now() - start > std::chrono::milliseconds(3)) {
                    all = false;
                    continue;
                }
                if (Get(rx, rz, true, false) != nullptr) loaded = true;
            }
        }
    }
    if (loaded) ++g_version;
    return all;
}

std::uint32_t Version() {
    return g_version;
}

void Tick(double now) {
    g_now = now;
    // One region a tick, the one that changed longest ago (at least 5 s ago).
    for (auto& [key, reg] : g_regions) {
        if (reg->dirty && now - reg->changedAt > 5.0) {
            Save(static_cast<int>(static_cast<std::int32_t>(key >> 32)), static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFFu)), *reg);
            return;
        }
    }
}

void Flush() {
    for (auto& [key, reg] : g_regions) {
        if (reg->dirty) {
            Save(static_cast<int>(static_cast<std::int32_t>(key >> 32)), static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFFu)), *reg);
        }
    }
}

}  // namespace luma::modules::world::mapstore
