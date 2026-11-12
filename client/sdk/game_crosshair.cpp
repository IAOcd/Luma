// =============================================================================
//  game_crosshair.cpp - Esconder la mira del juego (ver game_crosshair.h)
// =============================================================================
#include "sdk/game_crosshair.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/hooks.h"
#include "core/logger.h"
#include "sdk/memory.h"
#include "sdk/scanner.h"

namespace luma::sdk::gamecrosshair {

namespace {

// mov rax,"oss_hair" (los 8 bytes del texto como número)
constexpr const char* kCrossHairText = "48 B8 6F 73 73 5F 68 61 69 72";
constexpr std::uintptr_t kFromFunctionStart = 0x35F;

// La función "render" de los renderers de Minecraft:
//   (renderer, MinecraftUIRenderContext&, IClientInstance&, UIControl&, int pass, RectangleArea&)
using RenderFn = void(__fastcall*)(void* self, void* ctx, void* client, void* control, int pass, void* rect);
RenderFn g_original = nullptr;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_hidden{false};
std::atomic<unsigned int> g_calls{0};

void __fastcall RenderDetour(void* self, void* ctx, void* client, void* control, int pass, void* rect) {
    hooks::ActiveCallGuard guard;
    g_calls.fetch_add(1, std::memory_order_relaxed);  // (the game's HUD is being drawn)
    if (g_hidden.load(std::memory_order_relaxed)) {
        return;  // la mira del juego no se dibuja: la de Luma la reemplaza
    }
    g_original(self, ctx, client, control, pass, rect);
}

}  // namespace

bool Init() {
    const ScanResult match = FindPattern(kCrossHairText);
    if (match.matches != 1) {
        log::Warn("SDK: no se encontró el dibujo de la mira del juego ({} coincidencias): no se podrá esconder.", match.matches);
        return false;
    }
    const std::uintptr_t start = match.address - kFromFunctionStart;
    // Comprobar el comienzo: push rbp / push r15 / push r14 / push r13 / push r12 / push rsi / push rdi / push rbx
    static const std::uint8_t kPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53};
    std::uint8_t bytes[sizeof(kPrologue)]{};
    if (!SafeCopy(start, bytes, sizeof(bytes)) || std::memcmp(bytes, kPrologue, sizeof(kPrologue)) != 0) {
        log::Warn("SDK: el dibujo de la mira del juego no empieza como se esperaba: no se podrá esconder.");
        return false;
    }
    void* target = reinterpret_cast<void*>(start);
    if (!hooks::Create(target, reinterpret_cast<void*>(&RenderDetour), reinterpret_cast<void**>(&g_original),
                       "juego: dibujar la mira") ||
        !hooks::Enable(target, "juego: dibujar la mira")) {
        return false;
    }
    g_ready.store(true);
    log::Info("SDK: dibujo de la mira del juego en juego+0x{:X}.", start - GameBase());
    return true;
}

bool Available() {
    return g_ready.load(std::memory_order_relaxed);
}

unsigned int RenderCalls() {
    return g_calls.load(std::memory_order_relaxed);
}

void SetHidden(bool hidden) {
    g_hidden.store(hidden, std::memory_order_relaxed);
}

}  // namespace luma::sdk::gamecrosshair
