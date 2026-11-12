// =============================================================================
//  debug_screen.cpp - Module "Debug screen" (F3) (see debug_screen.h)
// =============================================================================
#include "modules/hud/debug_screen.h"

#include "render/icons.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "core/tr.h"
#include "core/version.h"
#include "modules/module_manager.h"
#include "perf/frame_stats.h"
#include "perf/gpu_timer.h"
#include "perf/system_info.h"
#include "render/hud.h"
#include "render/rainbow.h"
#include "render/ui.h"
#include "sdk/dynlight.h"
#include "sdk/inventory.h"
#include "sdk/network.h"
#include "sdk/player.h"
#include "sdk/blocks.h"
#include "sdk/players.h"
#include "sdk/worldid.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace ui = render::ui;

namespace {

constexpr int kMaxLines = 24;

// A column of lines (no memory is asked for: fixed arrays).
struct Column {
    char text[kMaxLines][160];
    int count = 0;
    void Add(const char* fmt, ...) {
        if (count >= kMaxLines) return;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text[count], sizeof(text[count]), fmt, args);
        va_end(args);
        ++count;
    }
    void Gap() {
        if (count > 0 && count < kMaxLines && text[count - 1][0] != '\0') text[count++][0] = '\0';
    }
};

// Where you look, like Java's F3: "South (+Z)" (the game's yaw: 0 = south).
const char* Facing(float yaw) {
    float h = std::fmod(yaw + 180.0f, 360.0f);
    if (h < 0.0f) h += 360.0f;
    static const char* const kNames[] = {"North (-Z)", "East (+X)", "South (+Z)", "West (-X)"};
    return Tr(kNames[static_cast<int>(std::floor((h + 45.0f) / 90.0f)) % 4]);
}

int FloorDiv16(int v) {
    return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

}  // namespace

DebugScreen::DebugScreen()
    : Module("debug_screen", "Debug screen",
             "Like Java's F3: exact position, block, chunk, where you look, speed, the light where you stand, world, "
             "ping, players and your PC. Press F3 (or your key) to show it.",
             Category::Hud) {
    SetDefaultKeybind(VK_F3);
}

const char* DebugScreen::IconSvg() const {
    return render::icons::kTerminal;
}

void DebugScreen::OnEnable() {
    perf::gputimer::Want(perf::gputimer::kDebugScreen, true);
    nextSlow_ = 0.0;
}

void DebugScreen::OnDisable() {
    perf::gputimer::Want(perf::gputimer::kDebugScreen, false);
}

ImVec2 DebugScreen::OnRenderHud(const HudFrame& frame) {
    const double now = rainbow::Seconds();
    if (now >= nextSlow_) {
        // The slow facts, once a second.
        nextSlow_ = now + 1.0;
        std::vector<sdk::players::Player> list;
        players_ = sdk::players::Read(list, false) ? static_cast<int>(list.size()) : 0;
        world_.clear();
        sdk::players::WorldName(world_);
    }

    Column left, right;
    // --- Performance ---
    if (showPerformance_) {
        left.Add("Luma %s  |  Minecraft %s", kVersion, perf::sysinfo::GameVersion().c_str());
        const perf::Recent recent = perf::RecentAverage(60);
        const double fps = recent.frameMs > 0.0 ? 1000.0 / recent.frameMs : 0.0;
        const float gpu = perf::gputimer::LastFrameMs();
        if (gpu >= 0.0f && recent.frameMs > 0.0) {
            left.Add(Tr("%.0f fps  |  %.2f ms  |  GPU %.2f ms (%.0f%%)  |  Luma %.2f ms"), fps, recent.frameMs, gpu,
                     std::min(100.0, gpu / recent.frameMs * 100.0), recent.costUs / 1000.0);
        } else {
            left.Add(Tr("%.0f fps  |  %.2f ms  |  Luma %.2f ms"), fps, recent.frameMs, recent.costUs / 1000.0);
        }
        left.Gap();
    }
    // --- Position ---
    sdk::player::Snapshot p;
    const bool havePlayer = sdk::player::Read(p);
    if (showPosition_) {
        if (havePlayer) {
            const int bx = static_cast<int>(std::floor(p.feet[0]));
            const int by = static_cast<int>(std::floor(p.feet[1]));
            const int bz = static_cast<int>(std::floor(p.feet[2]));
            left.Add("XYZ: %.3f / %.5f / %.3f", p.feet[0], p.feet[1], p.feet[2]);
            left.Add(Tr("Block: %d %d %d  [%d %d %d in the chunk]"), bx, by, bz, bx & 15, by & 15, bz & 15);
            left.Add(Tr("Chunk: %d %d  |  sub-chunk %d"), FloorDiv16(bx), FloorDiv16(bz), FloorDiv16(by));
            float yaw = std::fmod(p.yaw, 360.0f);
            if (yaw > 180.0f) yaw -= 360.0f;
            if (yaw < -180.0f) yaw += 360.0f;
            left.Add(Tr("Facing: %s  (%.1f / %.1f)"), Facing(p.yaw), yaw, p.pitch);
            const float vx = p.eye[0] - p.eyeOld[0], vy = p.eye[1] - p.eyeOld[1], vz = p.eye[2] - p.eyeOld[2];
            const float speed = std::sqrt(vx * vx + vz * vz) * 20.0f;  // blocks per tick -> per second
            left.Add(Tr("Speed: %.2f m/s horizontal  |  %.2f m/s vertical"), speed, vy * 20.0f);
            // The real light where you stand (asked to the game on its own thread; it arrives a frame later).
            sdk::dynlight::RequestLight(bx, by, bz);
            int sky = 0, block = 0;
            if (sdk::dynlight::LastLight(sky, block)) {
                left.Add(Tr("Light: %d  (sky %d, blocks %d)"), std::max(sky, block), sky, block);
            }
            left.Add(Tr("Age: %d ticks (%.1f s)"), p.ticks, p.ticks / 20.0f);
        } else {
            left.Add("%s", Tr("No player yet (join a world)"));
        }
        left.Gap();
    }
    // --- World ---
    if (showWorld_) {
        sdk::players::Server server;
        const bool online = sdk::network::OnServer();
        if (online && sdk::players::ReadServer(server)) {
            left.Add(Tr("Server: %s:%d"), server.address.c_str(), server.port);
        } else if (online) {
            left.Add("%s", Tr("Server: (address not available)"));
        } else {
            left.Add(Tr("Local world: %s"), world_.empty() ? "?" : world_.c_str());
            // The seed: only in your own worlds (on a server it would help find
            // structures and ores: an advantage).
            std::int64_t seed = 0;
            if (sdk::worldid::Seed(seed)) left.Add(Tr("Seed: %lld"), static_cast<long long>(seed));
        }
        const std::string biome = sdk::blocks::PlayerBiome();
        if (!biome.empty()) left.Add(Tr("Biome: %s"), biome.c_str());
        if (online) left.Add(Tr("Ping: %d ms"), sdk::network::PingMs());
        left.Add(Tr("Players: %d"), players_);
        sdk::inventory::Item item;
        const int slot = sdk::inventory::SelectedSlot();
        if (havePlayer && slot >= 0 && sdk::inventory::ReadSlot(slot, item) && item.count > 0) {
            char name[96];
            sdk::inventory::PrettyName(item.id, name, sizeof(name));
            if (item.maxDamage > 0) {
                left.Add(Tr("Holding: %s x%d  |  durability %d/%d"), name, item.count, item.maxDamage - item.damage,
                         item.maxDamage);
            } else {
                left.Add(Tr("Holding: %s x%d  (%s)"), name, item.count, item.id);
            }
        }
    }
    // --- Your PC (right column) ---
    if (showSystem_) {
        right.Add("%s  |  x64", perf::sysinfo::WindowsName().c_str());
        right.Add(Tr("CPU: %s (%d threads)"), perf::sysinfo::CpuName().c_str(), perf::sysinfo::CpuThreads());
        if (!perf::sysinfo::GpuName().empty()) right.Add(Tr("GPU: %s"), perf::sysinfo::GpuName().c_str());
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        right.Add(Tr("Screen: %.0f x %.0f  |  DirectX 12"), display.x, display.y);
        const perf::sysinfo::Memory& mem = perf::sysinfo::MemoryNow();
        right.Add(Tr("Minecraft memory: %.0f MB"), mem.gameMb);
        if (mem.totalMb > 0.0) {
            right.Add(Tr("PC memory: %.1f / %.1f GB (%.0f%%)"), mem.usedMb / 1024.0, mem.totalMb / 1024.0,
                      mem.usedMb / mem.totalMb * 100.0);
        }
        int on = 0, total = 0;
        for (const auto& m : Manager::All()) {
            ++total;
            if (m->IsEnabled()) ++on;
        }
        right.Add(Tr("Luma: %d of %d modules on"), on, total);
    }

    // --- Draw: each line on its own dark strip, like Java ---
    ImDrawList* draw = frame.draw;
    const rh::Metrics m = rh::Measure(size_);
    const float u = m.px;
    const float padX = 2.0f * u, padY = 1.0f * u;
    const float lineH = m.capHeight + padY * 2.0f + 1.0f * u;
    const ImU32 color = hudColors_ ? rh::TextColor() : IM_COL32(240, 240, 240, 255);
    const ImU32 strip = IM_COL32(16, 16, 18, 120);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto drawColumn = [&](const Column& col, bool alignRight) {
        float y = 3.0f * u;
        for (int i = 0; i < col.count; ++i) {
            const char* text = col.text[i];
            if (text[0] != '\0') {
                const float w = rh::TextSize(m, text).x;
                const float x = alignRight ? display.x - 3.0f * u - w - padX * 2.0f : 3.0f * u;
                if (background_) {
                    draw->AddRectFilled(ImVec2(std::floor(x), std::floor(y)),
                                        ImVec2(std::floor(x + w + padX * 2.0f), std::floor(y + lineH - 1.0f * u)), strip);
                }
                rh::Text(draw, m, ImVec2(x + padX, y + padY - m.capTop), text, color);
            }
            y += text[0] != '\0' ? lineH : lineH * 0.5f;
        }
    };
    drawColumn(left, false);
    drawColumn(right, true);
    return ImVec2(0, 0);
}

bool DebugScreen::DrawSettings() {
    bool changed = false;
    changed |= ui::Slider("Text size", &size_, 0.6f, 1.6f, "%.2f");
    changed |= ui::Toggle("Dark strip behind each line", &background_);
    changed |= ui::Toggle("Use the HUD's text color", &hudColors_);
    ui::Section("Show");
    changed |= ui::Toggle("Performance", &showPerformance_, "(FPS, GPU, Luma)");
    changed |= ui::Toggle("Position", &showPosition_, "(XYZ, block, chunk, facing, speed, light)");
    changed |= ui::Toggle("World", &showWorld_, "(server or world, ping, players, item)");
    changed |= ui::Toggle("Your PC", &showSystem_, "(Windows, CPU, GPU, memory)");
    ui::Hint("Press its key (F3 by default) to show or hide it. Time of day and the block you look at aren't available yet.");
    return changed;
}

void DebugScreen::SaveSettings(nlohmann::json& out) const {
    out["size"] = size_;
    out["background"] = background_;
    out["hudColors"] = hudColors_;
    out["showPerformance"] = showPerformance_;
    out["showPosition"] = showPosition_;
    out["showWorld"] = showWorld_;
    out["showSystem"] = showSystem_;
}

void DebugScreen::LoadSettings(const nlohmann::json& in) {
    size_ = std::clamp(in.value("size", 1.0f), 0.6f, 1.6f);
    background_ = in.value("background", true);
    hudColors_ = in.value("hudColors", false);
    showPerformance_ = in.value("showPerformance", true);
    showPosition_ = in.value("showPosition", true);
    showWorld_ = in.value("showWorld", true);
    showSystem_ = in.value("showSystem", true);
}

}  // namespace luma::modules::hud
