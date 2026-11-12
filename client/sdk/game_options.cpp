// =============================================================================
//  game_options.cpp - Reading the game's own settings (see game_options.h)
// =============================================================================
//  WHAT ONE GETTER LOOKS LIKE (1.26.6x; bytes from the start of the function):
//
//      +0   sub rsp, 38h ; (security cookie...)
//      +22  mov rax, [rcx]            ; the Options' table of functions
//      +26  mov rax, [rax+8]          ; its function 1: "give me setting N"
//      +31  lea rdx, [rsp+28h]        ; where it writes the answer
//      +33  mov r8d, N                ; N = the setting's number  <- we read it
//      +37  call rax                  ; -> a pointer to the setting ("holder")
//      +43  mov rcx, [rsp+28h]
//      +48  mov rax, rcx              ; LOOP: the setting may be "overridden"
//      +51  mov rcx, [rcx+8]          ;   by another one: follow the chain
//      +55  mov rcx, [rcx+238h]       ;   [[s+8]+238h] while it isn't 0
//      +62  test rcx, rcx / jne LOOP
//      +67  movzx eax, byte [rax+10h] ; the value (a bool) at +10h
//
//  We read the 8, the 238h and the 10h from the getters themselves (so a
//  small change of those numbers in an update doesn't break it), and only
//  accept getters that read a bool like this.
//
//  "HIDE HUD" HAS NO GETTER of its own (the game reads it in a generic way),
//  so its number comes from the place where the game CREATES that setting:
//
//      movabs r15, "gfx_hide"         ; 49 BF 67 66 78 5F 68 69 64 65
//      mov [rbp+..], r15
//      mov dword [rbp+..], "ehud"     ; -> "gfx_hidehud"
//      ... (its label "options.hidehud") ...
//      mov edx, N                     ; BA N  <- the number (0x1A5 in 1.26.63)
//      xor r8d, r8d                   ; 45 31 C0
//
//  The text bytes inside those instructions don't change between versions:
//  that's what makes it findable. Then we ask the game for setting N and read
//  it the same way the getters read theirs.
// =============================================================================
#include "sdk/game_options.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/logger.h"
#include "sdk/memory.h"
#include "sdk/scanner.h"

namespace luma::sdk::gameoptions {

namespace {

// The common part of all the getters, up to "mov r8d," (the number follows).
constexpr const char* kGetterFamily =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B 01 48 8B 40 08 48 8D 54 24 ?? 41 B8";
constexpr std::size_t kIdAt = 33;

constexpr const char kHideHudName[] = "gfx_hidehud";  // its name in options.txt
// Where the game creates it (see above); the number is in the next "mov edx, N".
constexpr const char* kHideHudCreate =
    "49 BF 67 66 78 5F 68 69 64 65 4C 89 BD ?? ?? ?? ?? C7 85 ?? ?? ?? ?? 65 68 75 64";
std::uint32_t g_hideHudId = 0;  // (found in Init)

struct Getter {
    std::uint32_t id = 0;
    std::uint8_t link1 = 0;   // the 8
    std::uint32_t link2 = 0;  // the 238h
    std::uint8_t value = 0;   // the 10h
};

std::vector<Getter> g_getters;  // (filled once in Init; read-only afterwards)

std::atomic<std::uintptr_t> g_seenOptions{0};  // the Options object we searched
std::atomic<std::uintptr_t> g_holder{0};       // "gfx_hidehud"'s holder (0 = unknown)
std::atomic<std::uint8_t> g_link1{0}, g_value{0};
std::atomic<std::uint32_t> g_link2{0};

// Reads a getter's numbers, if it has the shape above.
bool ReadGetter(std::uintptr_t at, Getter& g) {
    std::uint8_t code[72];
    if (!SafeCopy(at, code, sizeof(code))) return false;
    std::memcpy(&g.id, code + kIdAt, 4);
    // +37 call [rip+x] (FF 15) or call rax (FF D0): the game uses a guarded call.
    if (code[37] != 0xFF) return false;
    // +43 mov rcx,[rsp+..]   48 8B 4C 24 ..
    if (!(code[43] == 0x48 && code[44] == 0x8B && code[45] == 0x4C && code[46] == 0x24)) return false;
    // +48 mov rax,rcx        48 89 C8
    if (!(code[48] == 0x48 && code[49] == 0x89 && code[50] == 0xC8)) return false;
    // +51 mov rcx,[rcx+d8]   48 8B 49 d8
    if (!(code[51] == 0x48 && code[52] == 0x8B && code[53] == 0x49)) return false;
    g.link1 = code[54];
    // +55 mov rcx,[rcx+d32]  48 8B 89 d32
    if (!(code[55] == 0x48 && code[56] == 0x8B && code[57] == 0x89)) return false;
    std::memcpy(&g.link2, code + 58, 4);
    // +62 test rcx,rcx; +65 jne; +67 movzx eax, byte [rax+d8]   0F B6 40 d8
    if (!(code[62] == 0x48 && code[63] == 0x85 && code[64] == 0xC9 && code[65] == 0x75)) return false;
    if (!(code[67] == 0x0F && code[68] == 0xB6 && code[69] == 0x40)) return false;
    g.value = code[70];
    return g.link2 < 0x10000;
}

using GetOptionFn = void(__fastcall*)(void* options, void** out, std::uint32_t id);

// Asks the game for one setting, inside SEH (never crash the game).
bool SafeGetOption(GetOptionFn fn, void* options, std::uint32_t id, void** out) {
    __try {
        fn(options, out, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Search(void* options) {
    std::uintptr_t table = 0, fn = 0;
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(options), table) || !LooksLikePointer(table) ||
        !SafeRead(table + 8, fn) || fn == 0) {
        return;
    }
    void* holder = nullptr;
    if (!SafeGetOption(reinterpret_cast<GetOptionFn>(fn), options, g_hideHudId, &holder) ||
        !LooksLikePointer(reinterpret_cast<std::uintptr_t>(holder))) {
        log::Warn("Game options: the game didn't give setting 0x{:X}: F1 presses are counted instead.", g_hideHudId);
        return;
    }
    g_holder.store(reinterpret_cast<std::uintptr_t>(holder));
    const int now = HideHud();
    log::Info("Game options: \"{}\" is setting 0x{:X} (now {}): Luma's HUD follows it.", kHideHudName, g_hideHudId,
              now == 1 ? "on" : now == 0 ? "off" : "unreadable");
}

}  // namespace

bool Init() {
    const std::vector<std::uintptr_t> all = FindAllPatterns(kGetterFamily, 1024);
    for (const std::uintptr_t at : all) {
        Getter g;
        if (ReadGetter(at, g)) g_getters.push_back(g);
    }
    // Almost all the bool getters read the value the same way (in 1.26.63:
    // 85 of 86; the other is a setting of another kind). Take the way most of
    // them use, only if it's clearly the common one (3 of 4 or more).
    std::size_t best = 0, bestCount = 0;
    for (std::size_t i = 0; i < g_getters.size(); ++i) {
        std::size_t count = 0;
        for (const Getter& g : g_getters) {
            if (g.link1 == g_getters[i].link1 && g.link2 == g_getters[i].link2 && g.value == g_getters[i].value) ++count;
        }
        if (count > bestCount) {
            best = i;
            bestCount = count;
        }
    }
    const bool same = bestCount > 0 && bestCount * 4 >= g_getters.size() * 3;
    // The number of "Hide HUD".
    const ScanResult create = FindPattern(kHideHudCreate);
    if (create.address != 0 && create.matches == 1) {
        std::uint8_t code[0x180];
        if (SafeCopy(create.address, code, sizeof(code))) {
            for (std::size_t k = 27; k + 8 <= sizeof(code); ++k) {
                if (code[k] == 0xBA && code[k + 5] == 0x45 && code[k + 6] == 0x31 && code[k + 7] == 0xC0) {
                    std::uint32_t id = 0;
                    std::memcpy(&id, code + k + 1, 4);
                    if (id > 0 && id < 0x1000) g_hideHudId = id;
                    break;
                }
            }
        }
    }
    if (!same || g_hideHudId == 0) {
        log::Warn("Game options: \"{}\" not found ({} getters, {} yes/no, same way {}, create {}): F1 presses are "
                  "counted instead.",
                  kHideHudName, all.size(), g_getters.size(), same, create.matches);
        g_getters.clear();
        return false;
    }
    g_link1.store(g_getters[best].link1);
    g_link2.store(g_getters[best].link2);
    g_value.store(g_getters[best].value);
    log::Info("Game options: \"{}\" is setting 0x{:X} ({} of {} yes/no getters read settings the same way).",
              kHideHudName, g_hideHudId, bestCount, g_getters.size());
    return true;
}

void OnOptions(void* options) {
    const auto o = reinterpret_cast<std::uintptr_t>(options);
    if (o == 0 || g_getters.empty() || g_seenOptions.load(std::memory_order_relaxed) == o) return;
    g_seenOptions.store(o);
    g_holder.store(0);
    Search(options);
}

int HideHud() {
    std::uintptr_t s = g_holder.load(std::memory_order_relaxed);
    if (s == 0) return -1;
    // The same chain the game follows (see the top of this file).
    const std::uint8_t link1 = g_link1.load(std::memory_order_relaxed);
    const std::uint32_t link2 = g_link2.load(std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i) {
        std::uintptr_t a = 0, next = 0;
        if (!SafeRead(s + link1, a) || !LooksLikePointer(a) || !SafeRead(a + link2, next)) return -1;
        if (next == 0) break;
        if (!LooksLikePointer(next)) return -1;
        s = next;
    }
    std::uint8_t v = 0;
    if (!SafeRead(s + g_value.load(std::memory_order_relaxed), v) || v > 1) return -1;
    return v;
}

}  // namespace luma::sdk::gameoptions
