// =============================================================================
//  luma.cpp - Luma's lifecycle
// =============================================================================
//  The order of Luma's life inside the game:
//
//    1. Startup()      -> open the log, install hooks, etc.
//    2. WaitForUnload()-> wait for the user to press the unload key.
//    3. Shutdown()     -> undo EVERYTHING from step 1, in reverse order.
//
//  The golden rule: everything installed in Startup is removed in Shutdown.
//  If we left a hook in place and then the DLL disappeared from memory, the
//  game would jump to code that no longer exists and crash.
// =============================================================================
#include "core/luma.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/hooks.h"
#include "core/keys.h"
#include "core/logger.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/version.h"
#include "modules/module_manager.h"
#include "modules/performance/process_opts.h"
#include "render/notifications.h"
#include "render/sound.h"
#include "perf/benchmark.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/dx12_hooks.h"
#include "render/input.h"
#include "render/item_icons.h"
#include "render/assets.h"
#include "render/loading.h"
#include "render/overlay.h"
#include "sdk/mouse_grab.h"
#include "sdk/mouse_queue.h"
#include "sdk/camera.h"
#include "sdk/draw3d.h"
#include "sdk/hud_ui.h"
#include "sdk/game_crosshair.h"
#include "sdk/blocks.h"
#include "sdk/dynlight.h"
#include "sdk/skin_apply.h"
#include "sdk/paperdoll.h"
#include <nlohmann/json.hpp>

#include "core/cloud.h"
#include "core/lyrics.h"
#include "core/audio_viz.h"
#include "core/spotify.h"
#include "core/media.h"
#include <filesystem>
#include "sdk/memory.h"
#include "sdk/watch.h"
#include "core/tr.h"
#include "core/stats.h"
#include "i18n.h"
#include "render/skin_heads.h"
#include "sdk/network.h"
#include "sdk/player.h"

namespace luma {

namespace {

// The key to unload Luma (End by default) is in config::g_unloadKey and
// is changed from Settings.

// How often we check the key. 50 ms is unnoticeable for a person and
// uses practically no CPU (the thread spends 99.9% of the time asleep).
constexpr DWORD kPollIntervalMs = 50;

// In Debug we open a console to see the log live. Not in Release.
// NDEBUG is a "macro" the compiler only defines in Release.
#ifdef NDEBUG
constexpr bool kOpenConsole = false;
#else
constexpr bool kOpenConsole = true;
#endif

// A named lock at the Windows level (a named "mutex"). It's used to detect
// whether another copy of Luma is already running inside this same game.
HANDLE g_instanceMutex = nullptr;

// Were the DirectX hooks installed correctly?
bool g_hooksReady = false;

// Was the config loaded? (If not, it is NOT saved when unloading, so the
// file isn't overwritten with empty values.)
bool g_configLoaded = false;

// Tries to "reserve" the Luma name for this process.
// Returns false if another copy of Luma already had it.
bool AcquireInstance() {
    // The name includes the process ID (PID), so two games open at the same
    // time don't block each other. "Local\" = only in this Windows session.
    wchar_t name[64];
    std::swprintf(name, std::size(name), L"Local\\Luma_Instance_%lu", GetCurrentProcessId());

    g_instanceMutex = CreateMutexW(nullptr, FALSE, name);
    if (g_instanceMutex == nullptr) {
        return true;  // We couldn't check; we carry on (it's no reason not to load).
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return false;
    }
    return true;
}

void ReleaseInstance() {
    if (g_instanceMutex != nullptr) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
}

// Does the window that has focus right now belong to this process (the game)?
// That way the unload key doesn't trigger if you press "End" while typing in
// another program with Minecraft minimized.
bool IsGameWindowFocused() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

// Path of the game's executable, only to write it in the log.
std::string GameExePath() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return paths::ToUtf8(std::wstring(buffer, length));
}

bool Startup(HMODULE module) {
    log::Init(paths::LogsDir(), kOpenConsole);
    log::Info("{} {} loaded (DLL at 0x{:X})", kName, kVersion,
              reinterpret_cast<std::uintptr_t>(module));
    log::Info("Game: {} (PID {})", GameExePath(), GetCurrentProcessId());
    log::Info("Log: {}", paths::ToUtf8(log::CurrentFile()));

    if (!AcquireInstance()) {
        log::Warn("Another copy of Luma is already active in this game; this copy will do nothing.");
        return false;
    }

    // --- Overlay (Phase 2) ---------------------------------------------------
    // If any of this fails, Luma stays loaded without the overlay: what matters
    // is not crashing the game. The reason ends up in the log.
    perf::Init();
    if (hooks::Init()) {
        const bool dx12 = render::dx12::CreateHooks();
        // Without these hooks the menu works, but the game would keep moving the
        // camera with the menu open. It's no reason not to load.
        if (!render::input::CreateHooks()) {
            log::Warn("Input hooks unavailable: with the menu open, the mouse will move the camera.");
        }
        g_hooksReady = dx12 && hooks::EnableAll();
    }
    if (g_hooksReady) {
        log::Info("DirectX 12 hooks active. Waiting for the first frame...");
    } else {
        log::Warn("The overlay couldn't be enabled; Luma stays loaded without it.");
    }

    // From here on, the loading screen shows each step (the overlay already
    // draws as soon as the first frame arrives, in parallel with this thread).
    render::loading::Begin();
    render::assets::Decode();  // the logo (uploaded to the GPU on the next frame)
    render::loading::SetStep(Tr("Loading settings..."), 0.10f);

    // --- Modules and configuration (Phase 3) --------------------------------------
    // First the modules are created (all turned off) and then the config turns
    // on the ones that were on and gives them their options and keys.
    {
        std::lock_guard lock(StateMutex());
        modules::Manager::RegisterAll();
    }
    config::LoadAtStartup();
    g_configLoaded = true;
    // Language: the one you picked in the launcher (both use the same translations).
    {
        std::ifstream file(paths::DataDir() / L"launcher.json");
        const nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
        if (json.is_object()) {
            i18n::SetLanguage(i18n::LanguageFromCode(json.value("language", std::string("en")).c_str()));
        }
    }
    sound::Init();  // after the config: it uses the saved volume
    log::Info("{} modules registered. Active profile: {}.", modules::Manager::All().size(),
              config::ActiveProfile());

    // --- SDK (game memory) -------------------------------------------------------
    // The functions the game uses to grab and release the mouse. If they aren't
    // found, only the camera blocking with the menu open is lost.
    if (g_hooksReady) {
        sdk::mouse::Init([](int step, int total) {
            static const char* const kSteps[] = {"Finding game functions (1/5)...",
                                                 "Finding game functions (2/5)...",
                                                 "Finding game functions (3/5)...",
                                                 "Finding game functions (4/5)...",
                                                 "Finding game functions (5/5)...",
                                                 "Connecting to the game..."};
            render::loading::SetStep(Tr(kSteps[std::min(step, 5)]),
                                     0.2f + 0.7f * static_cast<float>(step) / static_cast<float>(total));
        });
    }
    // The mouse action queue: what really stops the game from receiving
    // clicks and movement with the menu open (see sdk/mouse_queue.h).
    if (g_hooksReady) {
        render::loading::SetStep(Tr("Connecting to the game's mouse..."), 0.95f);
        sdk::mousequeue::Init();
        // The local player (Phase 6): captured when the mouse moves in a world.
        sdk::player::Init();
        sdk::network::Init();  // ping and server detection
        sdk::camera::Init();   // gamma, perspective, camera (Phase 8)
        sdk::draw3d::Init();   // drawing inside the world (outline, hitboxes)
        sdk::paperdoll::Init();  // moving the game's HUD paper doll
        sdk::hudui::Init();      // movable scoreboard without flicker (UIControl::updatePos)
        sdk::skinapply::Init();  // Equip changes the skin instantly
        sdk::gamecrosshair::Init();  // hiding the game's crosshair (crosshair module)
        sdk::dynlight::Init();       // dynamic lights (dynamic lights module)
        sdk::blocks::Init();         // the biome of each block (map colors)
        std::lock_guard lock(StateMutex());
        modules::Manager::DisableUnavailable();
    }
    render::loading::Finish();
    stats::Begin();  // one more session (for the launcher)
    cloud::Start();  // "I'm using Luma" + who else uses it (on its own thread)

    const std::string menuKey = keys::Name(config::g_menuKey.load());
    notify::Push(notify::Kind::Info, TrFormat("{} loaded · press {} to open the menu", kName, menuKey));
    log::Info("Ready. Menu: {}. Unload: {} (with the game in the foreground).", menuKey,
              keys::Name(config::g_unloadKey.load()));
    return true;
}

// Writes the performance summary of the last 30 s to the log.
// Runs on THIS thread (not the render one), so writing to disk doesn't
// add any stutter to the game.
void LogPerformanceSummary() {
    const perf::Summary s = perf::TakeSummary();
    if (!s.valid) {
        return;
    }
    if (s.closedFrames > 0) {
        log::Info("Performance ({:.0f} s): {:.0f} average FPS | Luma cost with the menu closed:"
                  "{:.4f} ms average, {:.4f} ms max ({} frames)",
                  s.seconds, s.averageFps, s.averageCostUs / 1000.0, s.maxCostUs / 1000.0,
                  s.closedFrames);
    } else {
        log::Info("Performance ({:.0f} s): {:.0f} average FPS (menu open the whole period)",
                  s.seconds, s.averageFps);
    }
}

void WaitForUnload() {
    constexpr ULONGLONG kSummaryEveryMs = 30'000;
    perf::TakeSummary();  // Start counting from now.
    // We use the real clock (GetTickCount64) instead of adding "50 ms" per loop:
    // Sleep(50) on Windows usually lasts a bit longer (~62 ms), and the error adds up.
    ULONGLONG lastSummary = GetTickCount64();

    bool windowLocated = !g_hooksReady;

    // GetAsyncKeyState returns a number whose highest bit (0x8000) is 1 if the
    // key is pressed right now. The unload key is read from the config on
    // every loop (it can be changed from Settings).
    while (!(IsGameWindowFocused() &&
             (GetAsyncKeyState(config::g_unloadKey.load(std::memory_order_relaxed)) & 0x8000))) {
        Sleep(kPollIntervalMs);
        // Find the game window's object as soon as the game is in the
        // foreground (so the menu blocks the camera from the start).
        if (!windowLocated && IsGameWindowFocused()) {
            windowLocated = sdk::mouse::LocateWindow(GetForegroundWindow()) || !sdk::mouse::Available();
        }
        config::SaveIfDirty();  // autosave (only if something changed)
        render::input::PollHolds();             // toggle sprint/sneak: game menus and focus
        perf::benchmark::Poll();
        sdk::network::Poll();                   // ping of the connection to the server
        stats::Poll();
        sdk::skinapply::Poll();                 // did you equip another skin? (reads its PNG)
        render::skinheads::Poll();              // skin faces requested by the menu (Closet)                          // time played and player name (for the launcher)
        render::icons_items::Poll();            // item icons requested by the HUD                // finish tests (calculations and CSV outside the render)
        modules::performance::PollRenderThread();  // render thread priority
        {
            std::lock_guard lock(StateMutex());
            modules::Manager::Tick();  // modules that check something every 50 ms (durability warning...)
        }
        if (GetTickCount64() - lastSummary >= kSummaryEveryMs) {
            lastSummary = GetTickCount64();
            LogPerformanceSummary();
        }
    }
    log::Info("Unload key pressed.");
}

void Shutdown() {
    // REVERSE order of Startup. Each step depends on the previous one:
    //
    //  1. BeginUnload: the detours stop touching our state and only
    //     call the original function.
    //  2. DisableAll: MinHook restores the original functions; nobody new
    //     enters our detours anymore.
    //  3. WaitForActiveCalls: wait for the ones already inside to leave.
    //  4. input::Uninstall: give the game back its original WndProc, and wait
    //     again in case a message was going through ours.
    //  5. overlay::Shutdown: nobody uses ImGui or DX12 from the game anymore; now
    //     it's safe to release them.
    //  6. hooks::Shutdown: release MinHook.
    //  7. Modules: turn them off and delete them (nobody draws them anymore).
    //
    //  Before all that, we save the config (in case there were pending changes).
    if (g_configLoaded) {
        config::SaveNow();
    }
    stats::End();
    cloud::Stop();
    audioviz::Shutdown();  // the visualizer's listening thread
    spotify::Shutdown();  // the Spotify thread (a request takes 8 s at most)
    lyrics::Shutdown();  // the lyrics thread (a request takes 8 s at most)
    media::Shutdown();  // the music thread (it waits at most a couple of seconds)
    sdk::camera::BeginUnload();
    sdk::paperdoll::BeginUnload();  // the paper doll goes back to its place
    sdk::network::BeginUnload();
    sdk::player::BeginUnload();
    sdk::mousequeue::BeginUnload();
    sdk::mouse::BeginUnload();  // if the menu was open, give the mouse back to the game
    render::dx12::BeginUnload();
    hooks::DisableAll();
    hooks::WaitForActiveCalls();
    render::input::Uninstall();
    hooks::WaitForActiveCalls();
    render::overlay::Shutdown();
    hooks::Shutdown();
    if (g_hooksReady) {
        log::Info("Overlay and hooks released.");
    }
    {
        std::lock_guard lock(StateMutex());
        modules::Manager::Shutdown();
    }
    notify::Clear();
    sound::Shutdown();

    ReleaseInstance();
    log::Info("{} unloaded cleanly.", kName);
    log::Shutdown();  // Always last: that way everything before it ends up in the log.
}

}  // namespace

void Run(HMODULE module) {
    if (Startup(module)) {
        WaitForUnload();
    }
    Shutdown();
}

}  // namespace luma
