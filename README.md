# Luma

***English** · [Español](README.es.md)*

Luma is a quality-of-life client for **Minecraft Bedrock Edition on Windows** (the GDK version, `Minecraft.Windows.exe`).
It is a DLL that loads inside the official game and adds an overlay, a customizable HUD, visual and utility modules, and measurable performance optimizations.

Luma does **not** include anything that gives an advantage in multiplayer: no killaura, fly, reach, xray, ESP, autoclicker, etc.

> **Status:** phases 0 to 8 are nearly complete, plus the windowed launcher (parts 1 and 2).
> - **HUD:** keystrokes, FPS, CPS, clock, coordinates, compass, speed, ping, armor, held item, item counter, movable paper doll and watermark. Everything is edited in the HUD editor: drag it, resize it from the corners and line it up with "magnets".
> - **Visual:** custom crosshair, post-processing, fullbright, freelook, zoom (with cinematic modes), FOV, block outline and hitboxes, drawn inside the world.
> - **Performance:** frametime overlay, benchmark exportable to CSV and process optimizations, each one separate and measurable.
> - **Launcher:** a window with dark and light themes, a PLAY button that opens the game and injects Luma, stats, news, settings in English, Spanish and Portuguese, and optional Microsoft/Xbox sign-in.
> - **Menu:** opens with **L**. **End** unloads Luma without closing the game.
>
> **New in 0.3.0:** a new in-game menu (in English, Spanish and Portuguese), skins that change instantly without going through the Dressing Room, hand-painted crosshairs (8x8 to 64x64), rainbow colors, a Tab player list, a movable scoreboard, a radial menu and the Closet. The full notes are in `release/notes/0.3.0.json`.

---

## What you need to install

| Program | What it's for | How to install it |
|---|---|---|
| **Visual Studio 2022** (Community is free) | Brings the C++ compiler, which turns the code into `.exe` and `.dll` files | In the installer, tick the **"Desktop development with C++"** workload |
| **CMake** 3.24 or newer | Reads the `CMakeLists.txt` "recipes" and generates the Visual Studio project | `winget install Kitware.CMake`, or in the Visual Studio installer: "C++ CMake tools for Windows" |
| **Git** | Keeps the project's change history | `winget install Git.Git` |

You **don't need to install** the external libraries (MinHook, Dear ImGui, nlohmann/json): CMake downloads them by itself the first time you configure the project. That's why you need internet that first time.

## How to build

Open a terminal (PowerShell) in the project folder and run:

```
cmake --preset x64
cmake --build --preset release
```

- The first command **configures**: it downloads the dependencies and generates the Visual Studio project in `build/`.
- The second one **builds** in Release mode (optimized).

The result ends up in `build/bin/Release/`:

- `Luma.dll`
- `LumaLauncher.exe` (the windowed launcher)
- `LumaLauncherCli.exe` (the console version, for tests and scripts)

For a debug build (slower, but useful for hunting bugs), change the second command to `cmake --build --preset debug`.

**From Visual Studio:** you can also use *File → Open → Folder* and pick the project folder. Visual Studio detects the presets by itself.

If something breaks in a weird way, delete the `build/` folder and run both commands again. It's safe: everything in there is regenerated.

## How to use Luma

1. Double-click `build\bin\Release\LumaLauncher.exe`.
2. Press **PLAY**. If Minecraft isn't open, the launcher opens it, waits for it to load and injects Luma. Below the button it tells you what it's doing, or the error and what to do. When it's done, it minimizes.
3. The **Client Info** card shows the Luma and Minecraft versions, and whether the game and Luma are running right now.
4. **L** opens and closes the Luma menu. Known limitation: Luma doesn't know yet whether the game's chat is open, so typing an "l" in the chat opens the menu. Until that's fixed you can change the key in *Settings*, for example to Insert.
5. To remove Luma without closing the game: press **End** with the Minecraft window in the foreground. This can also be changed in *Settings*.

### The menu
- **"Liquid glass" style:** a single glass window that really blurs the game: the frame is copied and a Gaussian blur runs on the GPU (`client/render/gpu.cpp`). It has a dark and a light style, with Luma's colors (charcoal `#25282B` and ivory `#E9E6DF`).
- **Sidebar:**
  - the logo;
  - the module categories: All, HUD, Visual, Utility, World and Performance;
  - the *Theme*, *Profiles*, *Settings*, *Benchmark* and *Diagnostics* pages.
- **Top bar:** search (across all categories), profile picker and **Edit HUD**.
- **Module cards:** their own SVG icon, assigned key and switch. Clicking one opens its page, with description, switch, key and options. Modules can be shown as blocks or as a list.
- **Icons:** SVG "paths" drawn as vectors (`client/render/icons.h`). The ones starting with `F` are filled, like the Luma logo.
- **Toasts, sounds and animations:**
  - The glass sounds are **synthesized** at startup and played with XAudio2, so there are no third-party audio files.
  - Everything can be turned off in *Theme*.
- **Languages:** English, Spanish and Portuguese, the same as the launcher (see "Translations" below).
- **Loading screen** when injecting.

### The HUD (what you see while playing)
The style is shared by every element and is chosen in *Theme > On-screen HUD* (`client/render/hud.h`):
- square, dark, translucent boxes;
- text with the game's shadow;
- configurable opacity, corners, border and colors, including rainbow.

**"Pixel" font:** it's the pixel font that comes with your Minecraft (`Content\data\fonts\Mojangles.ttf`).
- Luma **reads it from your installation** at startup: it doesn't include or distribute it. If it can't find it, it uses Luma's own (Segoe UI).
- To look sharp, it's loaded at 8 exact sizes of its grid. That's why element sizes go in steps.

Everything is measured in **HUD pixels**, like the game: 1 HUD pixel = 2 screen pixels at 1080p with size 1x.

**HUD editor:** opens with *Edit HUD* (`client/render/hud_editor.cpp`). The menu hides and every active element gets an outline.

| Action | What it does |
|---|---|
| Drag | Move. It "snaps" to the edges and center of the screen and to the edges of other elements, with a guide line. |
| Shift while dragging | Move without magnets. |
| Wheel | Size, from 50% to 400%. |
| Right click | Back to the default position. |
| Esc or *Done* | Back to the menu. |

Positions are saved in the profile as **fractions of the screen** plus an **anchor**: the point of the element that stays fixed. The anchor is picked automatically from the third of the screen where you drop the element. That way an element stuck to the right grows to the left if its text gets longer, and everything works at any resolution.

**Where each value comes from (without reading game memory):**

| Module | Value |
|---|---|
| FPS | Luma's frame measurement (updated 4 times per second so it's readable). |
| CPS | The clicks in the **game's mouse queue** (`sdk/mouse_queue`), the same place the game reads them from. |
| Keystrokes | `GetAsyncKeyState`, only while the game window is in the foreground. |
| Clock | `GetLocalTime`. |

About CPS:
- We measured that the game doesn't always empty that queue after processing it. So a click isn't counted twice, Luma marks every counted click in a **padding** byte of the action: byte +11, which nothing uses.
- If the queue's signature isn't found, it falls back to Windows' `WM_LBUTTONDOWN` messages.

**Custom crosshair** (`modules/visual/crosshair.cpp`):
- **Hides the game's one:** the original crosshair is drawn by a function of the game's own UI. Luma finds it through the text "cross_hair" (`sdk/game_crosshair.cpp`) and doesn't let it draw while the module is on.
- **Two kinds:** a shape (cross, dot, circle) or a crosshair **painted** pixel by pixel in an 8x8, 16x16, 32x32 or 64x64 editor, with mirroring and a palette.
- **Colors:** the original ones, *Rainbow* or *Rainbow wave*. To keep it cheap, the painted crosshair is drawn as strips of identical pixels, computed when editing and not every frame.
- It hides by itself in the game's menus.

**Toggle sprint / sneak** (`render/input.h`, "held keys"):
- **How it holds the key:** the game keeps track of pressed keys through the window messages. Luma sends it a `WM_KEYDOWN` for Ctrl (or Shift) and holds back the real `WM_KEYUP`; on top of that, `GetAsyncKeyState` answers "pressed".
- **How it toggles:** by pressing the real key.
- **When it releases by itself:**
  - in the inventory, the chat and the pause screen, because the game "releases" the mouse (otherwise a held Shift would make every inventory click move items);
  - when the window loses focus; when it comes back, it presses again.

### Settings and profiles
- `%APPDATA%\Luma\config\luma.json` stores which profile is active.
- `%APPDATA%\Luma\config\profiles\<name>.json` stores a whole profile: active modules, their options and keys, the theme and Luma's keys.
- **Autosave:** it saves one second after the last change, and also when Luma unloads.
- **Safe writes:** it first writes to `.tmp` and then replaces the real file, so a power cut never leaves it half written.
- **Damaged file:** if a file is damaged, a `.corrupt.json` copy is kept, the defaults are used and a toast appears.

### Translations
- **Launcher:** the texts use keys (`"home.play"` → "PLAY"), in `shared/i18n.cpp`, one line per text with the three languages.
- **In game:** the code is written in English and `Tr("text")` (`client/core/tr.h`) looks it up in `client/core/tr_table.inc` and `tr_menu.inc`, one line per text: `{"Español", "English", "Português"}`. A row can be found by its English or its Spanish text. If a text isn't in the table, it's shown as written.

### The launcher

- **Its own window:** Win32 + Direct3D 11 + Dear ImGui (`launcher/src/gui/`). It moves from the top bar, resizes from the edges and has Windows 11's shadow and rounded corners.
- **Themes:** dark and light, taken from the two colors of the logo. Switch them with the sun or moon button at the top right.
- **Languages:** English, Spanish and Portuguese, chosen in *Settings*. The in-game menu uses the same language.
- **Config:** `%APPDATA%\Luma\launcher.json` stores the theme, the language and what to do after PLAY.
- **Stats:** Luma.dll records the sessions (every time it loads) and the time played with Luma loaded in `stats.json`. It saves every minute and when unloading. The launcher shows them in "Your Stats".
- **Your name without an account:** Luma.dll saves your player name (in Bedrock it's your gamertag) to `profile.json` when you join a world.
- **Microsoft account (optional):** *Settings → Account*. It opens your browser on Microsoft's page (OAuth 2.0 with PKCE); your password never goes through Luma. Then it asks for the Xbox Live token, which brings your gamertag and picture. The session is kept in `account.dat`, encrypted with DPAPI: only your Windows user on this PC can read it. Code in `launcher/src/account.cpp`. The "Luma Launcher" app is registered in Azure; its identifier is in the code and is public by design.
- **Start with Windows:** off by default. If you turn it on, it adds the launcher to the `Run` key of your user's registry and starts minimized. Turning it off removes it.
- **Performance:** it draws nothing while minimized.

`LumaLauncherCli.exe` (console) options:

| Option | What it does |
|---|---|
| `--no-pause` | Doesn't wait for you to press Enter at the end (useful for scripts) |
| `--skip-gdk-check` | Skips the GDK version check. Only use it if detection fails and you're sure you have the GDK version |

**Logs:** every time Luma loads it creates a new file in `%APPDATA%\Luma\logs\` and keeps the last 10. To open the folder, paste `%APPDATA%\Luma\logs` into File Explorer's address bar.

**Debug console:** if you use the Debug build (`build\bin\Debug\`), a window with the live log opens too. Its close button is disabled on purpose: closing that window would close the whole game. It disappears by itself when Luma unloads.

## Project structure

```
CMakeLists.txt        Main recipe: shared options and subfolders
CMakePresets.json     Configuration shortcuts (x64, release, debug)
cmake/
  Dependencies.cmake  Which external libraries are downloaded and at which version
launcher/             LumaLauncher.exe (window) and LumaLauncherCli.exe (console)
  src/flow.cpp        What happens when you press PLAY (open the game, inject, confirm)
  src/gui/            The window: platform, themes and interface
shared/               What the launcher and Luma.dll both use: translations, skin pack
client/               Luma.dll
  core/               Startup, hooks, logger, config, events, translations
  render/             Overlay, HUD editor, drawing
  modules/            One file per module
  sdk/                Everything that depends on the game's internal memory
  perf/               Measurement, benchmark and optimizations
release/              Release notes and supported Minecraft versions
```

## How to update the signatures (when the game updates)
If after a Minecraft update the *"Signature not found"* toast appears, the log says which one failed. For the mouse one:
1. Put the old pattern from `client/sdk/mouse_grab.cpp` aside and find the equivalent function in the new version, repeating the method described below in "Input with the menu open":
   - record the call stacks of `ShowCursor` and `ClipCursor` when pausing;
   - disassemble the functions that show up with `dumpbin /disasm /range:...`.
2. Write the new signature with the bytes from the start of the function. Put `??` on everything that is an address or an offset.
3. Check that it appears **exactly once** in the executable.

## How to add a module

1. Create `client/modules/<category>/<name>.h` and `.cpp` with a class that inherits from `luma::modules::Module`. Use `modules/hud/hud_modules.cpp` (FPS, clock...) as an example.
2. In the constructor:
   - pass an internal `id` (no spaces or accents, and it must never change: it's the key in the config), the visible name, the description and the category;
   - if it draws on screen, call `SetDefaultLayout(...)` with its default position.
3. Override only what you need:
   - `HasHud()` + `OnRenderHud(frame)`: draw every frame while it's on. Draw with its corner at `frame.pos`, use `render::hud` for the style and **return its size**. The manager places it and the HUD editor moves it without the module doing anything.
   - `IsHudElement()`: return `false` if it draws but must not be moved, like the crosshair.
   - `CanScale()`: return `false` if its size can't be changed (like the scoreboard, whose text the game draws).
   - `OnEnable()` / `OnDisable()`.
   - `HasSettings()` + `DrawSettings()`: its options in the menu. Return `true` if something changed, so it gets saved.
   - `SaveSettings()` / `LoadSettings()`: its options in the profile. The manager saves the HUD position.
   - `IconSvg()`: its icon (a 24x24 SVG path).
4. Add it to the list in `client/modules/registry.cpp` and its `.cpp` to `client/CMakeLists.txt`.
5. Add its visible texts to `client/core/tr_table.inc` so they're translated.

The menu, search, keys, saving and the HUD editor work by themselves. A module that's off runs no code per frame, because the render only walks the list of active modules with a HUD.

## Benchmark and optimizations (Phase 4)

### Benchmark (*Benchmark* page)
1. Pick a name, duration (10-300 s) and mode.
2. Press *Start*. The menu closes, there's a 5 s countdown (so there are no toasts on screen, which cost something) and it records every frame. If you open the menu while it records, it's canceled.
3. When it finishes, a toast shows the result. The files end up in `%APPDATA%\Luma\benchmarks\`:
   - `summary.csv`: one row per test, to compare;
   - `<date>_<name>.csv`: one row per frame, to chart.

   They use your Windows list separator and decimal comma, so Excel opens them right the first time.

What it measures:

| Number | Meaning |
|---|---|
| Average FPS | Frames / seconds. |
| 1% low / 0.1% low | The slowest 1% (or 0.1%) of frames, averaged and turned into FPS. They show the stutters the average hides. |
| Frametime, p99, max | In ms. p99 = 99% of the frames took less than that. |
| Luma cost | Average per frame. |

**Modes:**
- **With my HUD:** measures the game with your setup, including what drawing the HUD costs.
- **Luma idle:** during the test Luma draws nothing. Its cost drops to ~0.001 ms per frame, so it's the most honest "without Luma" baseline that can be measured from inside the game. For a real external measurement without Luma, there are tools like PresentMon or CapFrameX.

**To get comparable numbers** ("without Luma / with Luma / with each optimization"), stay still in the same place looking at the same thing, and run:

| # | Mode | Optimizations |
|---|---|---|
| 1 | Luma idle | none (≈ without Luma) |
| 2 | With my HUD | none (= Luma's cost) |
| 3, 4... | Luma idle | **one** optimization per test |

If an optimization doesn't improve the numbers, it's dropped (project rule).

### Process optimizations (*Performance* category)
Each one is a separate module: it's turned on by itself, measured by itself and undone when you turn it off or unload Luma (`client/modules/performance/process_opts.cpp`).

| Module | What it does | Careful |
|---|---|---|
| Process priority | `SetPriorityClass`: "above normal" or "high". | "High" can slow down other programs. Never "realtime", which can freeze the PC. |
| 1 ms timer | `timeBeginPeriod(1)` plus the Windows 11 option to keep it honored even when the window is covered. Frames can come out more even, especially with an FPS limit. | Uses a bit more battery. |
| No efficiency mode | Forbids Windows 11 from putting Minecraft in "EcoQoS" (slow cores), which it does with background windows. | — |
| Render thread priority | Raises only the thread that calls `Present`, not the whole game. | — |

What we deliberately **don't** do: "RAM cleaning", registry tricks, realtime priority and anything that can't be measured and undone.

### Windows tips
The *Benchmark* page **reads** how these options are set and explains how to change them:
- Game Mode.
- Hardware-accelerated GPU scheduling.
- Optimizations for windowed games.
- Power plan.
- Preferred GPU for Minecraft (matters on laptops with two GPUs).

It has buttons that open the matching Windows Settings page. **Luma never changes them by itself.**

## Technical decisions

### Dependencies with FetchContent (not vcpkg)
`FetchContent` comes with CMake, so there's nothing extra to install.
Every library has a **pinned version** in `cmake/Dependencies.cmake`, so a third-party update never breaks the build by surprise.

| Library | Version | License | Use |
|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | 1.3.4 | BSD-2-Clause | Intercepting functions (hooks) |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.5 | MIT | Menu and HUD interface |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | Configuration files |

### Static C++ runtime (`/MT`)
The C++ support code stays inside `Luma.dll`. That way the DLL doesn't depend on external files or clash with the runtime version the game uses.

### x64 only
Minecraft GDK is a 64-bit process, and a 32-bit DLL can't load into it. CMake rejects other architectures with a clear error.

### Injection: `LoadLibraryW` + `CreateRemoteThread`
It's the simplest and best-known method: the launcher writes the path of `Luma.dll` into the game's memory and creates a thread there that loads it. It doesn't modify any game file. It's explained step by step in `launcher/src/injector.cpp`.

### Detecting the GDK version
- The old UWP version runs inside an *AppContainer* (the Store apps' sandbox) and GDK doesn't. If the process is in an AppContainer, the launcher rejects it.
- GDK games ship `MicrosoftGame.config` next to the executable. If that file is missing, the launcher rejects it too.
- The package name is **not** used: the GDK version is still called `Microsoft.MinecraftUWP` for compatibility.
- Install path: by default `C:\XboxGames\Minecraft for Windows\Content\`. From inside the game that same folder looks like `C:\Program Files\WindowsApps\Microsoft.MinecraftUWP_...`, because that's how Windows mounts GDK games. It's normal for the log to show that second path.

### DLL lifecycle
`DllMain` only creates a thread and all the work happens in that thread (`client/core/luma.cpp`). The steps are:
- **Startup** (`Startup`).
- **Wait** for the End key, checking it every 50 ms.
- **Shutdown** (`Shutdown`), which undoes everything in reverse order.
- **Exit** with `FreeLibraryAndExitThread`, which removes the DLL from memory and ends the thread in a single step.

If the game closes with Luma loaded, Luma does nothing in `DLL_PROCESS_DETACH`: touching locks at that moment could freeze the shutdown.

### Overlay graphics API: DirectX 12
Minecraft GDK loads `d3d12.dll` and draws with DirectX 12, except on some PCs with old GPUs, where it uses DirectX 11. Luma draws its overlay with DX12 using ImGui's official backends (`imgui_impl_dx12` + `imgui_impl_win32`). If it detects the game uses another API, it turns off only the overlay, writes it to the log and the game carries on normally. DX11 support (and an option to force it) is left for later, and will be measured with the benchmark.

**How it hooks in** (`client/render/dx12_hooks.cpp`):
1. For an instant, Luma creates a test DX12 swapchain on an invisible window and reads the addresses of `Present`, `Present1`, `ResizeBuffers` and `ResizeBuffers1` from its *vtable*. It also reads `ExecuteCommandLists` from a test command queue.
2. The positions inside the vtable are fixed by the COM ABI and never change. They're also Windows functions, not the game's, so they **don't depend on the Minecraft version** and need no signature scanning.
3. MinHook intercepts those functions:
   - **`ExecuteCommandLists`:** captures the game's *direct* queue, which DX12 needs to send work to the GPU.
   - **`Present`:** draws the menu on top of the frame.
   - **`ResizeBuffers`:** releases and recreates the resources when the window size changes.

**Cost with the menu closed:** the `Present` hook returns right away after reading a couple of variables. ImGui doesn't run.

### Input with the menu open
With the menu open, the game must receive **nothing**: no camera, no clicks, no keys. There are three layers, and all of them are needed.

**1. Windows** (`client/render/input.cpp`). These are Windows functions, stable across game versions:
- **Its own WndProc:** detects the menu key (by message, without polling the keyboard every frame), Esc and the module keys. With the menu open, keyboard and mouse go to ImGui.
- **`PeekMessageW`:** the game sometimes peeks at messages before handing them to the window. With the menu open, Luma processes them and the game gets a `WM_NULL`.
- **Release messages:** key and button *release* messages always go through. The game keeps track of pressed keys with them, and if the "release W" got lost it would keep walking. When the menu opens, Luma also tells the game that everything that was pressed got released.
- **`GetAsyncKeyState` / `GetKeyState`:** with the menu open, the game sees every key as not pressed.
- **`ClipCursor`:** with the menu open, the game can free the cursor, but not lock it.

**2. The game's client releases the mouse** (`client/sdk/mouse_grab.cpp`). It's the same thing pausing does: it shows the cursor and stops treating the mouse as "in game". There are two levels:
- **Window** (platform layer):
  - *Grab*: hides the cursor, locks it in the center and sets `window+0x398 = 1`.
  - *Release*: does the opposite.
- **Client** (`ClientInstance`): its flag `client+0x1F8` says whether the mouse is "in game". Its *grab* (`game+0x8BF820`) and *release* (`game+0x8BFA10`) functions change that flag and call the window ones. Luma uses these, like other clients do.
- **Objects without pressing Esc:**
  - The client one is captured the first time the game calls *grab*, which it does continuously.
  - The window one is found at load time, from the global variable of the game's WndProc, at `[[context+0x8]+0x78]`.

**3. The mouse action queue** (`client/sdk/mouse_queue.cpp`). It's the key for clicks:
- **One central list:** the game has a global `MouseDevice` object with a list of pending mouse actions (movement, clicks and wheel), 20 bytes each. All mouse input, whether it comes from GameInput or from messages, ends up there.
- **Processed every tick:** a game function (`game+0x427EF0`) walks it every tick.
- **What Luma does:** it hooks that function and, with the menu open, **removes the clicks and the wheel** before they're processed. When the menu opens it adds "release left/right" to cut off actions in progress.
- **Why it isn't emptied entirely:** movements are let through. With the mouse released they don't turn the camera, and if they were deleted, the camera would jump when the menu closes because the game wouldn't update its position.
- **Memory:** nothing is ever added to the list's memory; it only writes inside the space it already has.

**Signatures:** all of them are found by scanning (`client/sdk/scanner.cpp`). The offsets (`+0x398`, `+0x3A0`, `+0x1F8`) and global variables are read from the matched instructions themselves. If a signature fails after an update, a toast appears and only that part of the blocking is lost.

**How we got here.** It took quite a while, so I'm writing it down in case it helps with another update:
- Blocking raw input, `GetCursorPos` and the GameInput methods **didn't work**: a diagnostic that counted each path showed the game doesn't use them for the camera or the clicks.
- The *grab/release* functions were found by recording the **call stack** of `ShowCursor`/`ClipCursor` when pausing and disassembling with `dumpbin /disasm`.
- The action queue was found by **studying the open-source clients [Latite](https://github.com/LatiteClient/Latite) and Flarial** to understand the idea. Luma's signatures are its own, taken from the disassembly of this game. Flarial's public ones don't match 1.26.63.
- Freezing `GetCursorPos` and putting the cursor back when closing **caused the camera jump**, because GameInput reads the cursor position. They were removed.

**Safe unload**, in this order:
1. The hooks stop touching Luma's state.
2. MinHook restores the original functions.
3. It waits until no game thread is still inside Luma's code.
4. The original WndProc is restored.
5. It waits for the GPU and releases ImGui and DX12.

**ImGui asserts:** in Debug, an internal ImGui error would close the game. `client/render/imgui_config.h` redirects it to the log.

### Performance measurement (`client/perf/`)
- **Clock:** `QueryPerformanceCounter`, which has ~100 ns resolution and costs ~20-30 ns to read.
- **Frametime:** the time between two calls to `Present`.
- **Luma cost:** the time from entering the `Present` hook until Luma's code finishes, not counting the game's original `Present`. Recording the measurement itself (a few tens of ns) is left out. The `ExecuteCommandLists` and `GetRawInputData` hooks aren't measured because, with the menu closed, they only read a variable.
- **History:** the last 512 frames in fixed-size arrays (a *ring buffer*), without allocating memory per frame. The *Diagnostics* graph and the HUD use it. The benchmark stores every frame of the test separately, in memory reserved when it starts.
- **Summary every 30 s in the log:** the average FPS and Luma's cost **only for frames with the menu closed** (average and max). Luma's main thread writes it, not the render thread, so writing to disk causes no stutter. That way the Phase 2 criterion can be checked without opening the menu, since opening it changes the measurement.
- **Initialization:** the overlay initializes only once, on the first frame. It's reported separately in the log and isn't counted as per-frame cost.

### Render fast path (Phase 3)
Every frame, the `Present` hook checks three atomic variables:
- is the menu open?
- is there any active module that draws?
- is there any toast on screen?

If all three are "no", it returns without touching ImGui. With 0 active modules, the measured cost was **~0.0009 ms per frame**; in Phase 2 it was 0.0002-0.0005 ms. The difference is under 1 µs, in the range of measurement noise.

Cost with active modules (menu closed, 1080p, measured in the log):

| Active modules | Cost per frame |
|---|---|
| Watermark | ~0.09 ms |
| 7 elements (keystrokes, FPS, CPS, clock, watermark, crosshair, performance overlay) | ~0.37 ms |

Almost all of that cost is fixed: preparing an ImGui frame and sending its commands to the GPU. Each element costs little. Optimizing it further is left for Phase 12 (for example, reusing the HUD geometry between frames if it didn't change).

### Threads and a single lock
- **Three threads touch Luma's state:**
  - the game's render thread, which draws;
  - the window thread, which receives keys and mouse;
  - Luma's main thread, which saves the config and waits for the unload key.
- **Lock:** every access to modules, config and ImGui goes through `luma::StateMutex()` (`client/core/state.h`).
- **Rule to avoid deadlocks:** never hold the lock while calling an original game or DirectX function.

## Phases

Progress:

- [x] 0. Environment
- [x] 1. Launcher + minimal DLL
- [x] 2. Overlay + measurement (cost with the menu closed: ~0.0003 ms per frame)
- [x] 3. Module system and config (cost with 0 modules: ~0.0009 ms per frame)
- [x] 4. Benchmark + process optimizations
- [x] 5. HUD editor + modules without game memory (FPS, CPS, keystrokes, clock, crosshair, toggle sprint/sneak)
- [x] 6. SDK (signature scanning, local player, inventory, entities, camera)
- [x] 7. Modules with game data (potion effects and chat improvements still missing)
- [x] 8. Camera and render (zoom, FOV, fullbright, freelook, block outline, hitboxes)
- [ ] 9. Render optimizations
- [ ] 10. Free cam
- [ ] 11. Minimap
- [ ] 12. Polish

## Automatic updates and how to publish a version

`LumaLauncher.exe` updates itself, like Flarial. When it opens, it downloads two small files from the latest GitHub Release: `latest.json` and its signature `latest.json.sig`.
- If there's a new version **and the signature is valid**, it downloads the new `.exe`, checks its SHA-256 hash, replaces itself and reopens.
- Without internet, it carries on normally.
- Only **official** builds update, the ones GitHub Actions builds. Yours, from `build/`, are never overwritten.
- Code: `launcher/src/updater.cpp`, `signature.cpp` and `net.cpp`.

**The signature.** There's an ECDSA P-256 key pair:
- The **public** key is inside the launcher (`launcher/src/signature.cpp`) and is only used to verify.
- The **private** key is in the repository's `LUMA_SIGNING_KEY` secret and in the author's backup. **It never goes in the code.** If it's lost, already installed launchers wouldn't accept updates signed with a different key.

**To publish a version:**
1. Change `kVersion` in `client/core/version.h`, and the `project(...)` version in `CMakeLists.txt`.
2. Write the release notes in `release/notes/<version>.json`, in English, Spanish and Portuguese.
3. If the supported Minecraft version changed, update `release/minecraft.json`.
4. Commit and push the changes. Then create the tag and push it:
   ```
   git tag v0.3.0
   git push origin v0.3.0
   ```
5. GitHub Actions (`.github/workflows/release.yml`) builds, assembles and signs `latest.json`, and creates the Release. You can watch it in the repo's **Actions** tab.

## License

Luma is free software under the **GNU General Public License v3.0** (see [`LICENSE`](LICENSE)). You can use, study, modify and share it. If you publish a modified version, it must also be GPL-3.0 with its source code available.

Third-party libraries, downloaded at build time, have their own licenses:
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause)
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT)
- [nlohmann/json](https://github.com/nlohmann/json) (MIT)

The ideas of how some of the game's internals work were studied in open-source clients such as Latite and Flarial. Luma's code is written from scratch.

## Disclaimer

Luma is an independent project. **It is not affiliated with, endorsed by or associated with Mojang Studios or Microsoft.** "Minecraft" is a trademark of Mojang Studios / Microsoft. Luma doesn't include or distribute game files: you need your own legitimate copy of Minecraft Bedrock for Windows.
