// =============================================================================
//  camera.cpp - Hooks de cámara y opciones (ver camera.h)
// =============================================================================
#include "sdk/camera.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "core/hooks.h"
#include "core/logger.h"
#include "perf/timer.h"
#include "sdk/memory.h"
#include "sdk/scanner.h"

namespace luma::sdk::camera {

namespace {

// --- Firmas (1.26.6x) ----------------------------------------------------------------
// getGamma y getPerspective son casi iguales: leen una opción por su número
// (0x32 = gamma, 0x03 = perspectiva). Ese número al final las distingue.
constexpr const char* kGetGamma =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B 01 48 8B 40 08 48 8D 54 24 ?? 41 B8 32 00 00 00";
constexpr const char* kGetPerspective =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B 01 48 8B 40 08 48 8D 54 24 ?? 41 B8 03 00 00 00";
constexpr const char* kUpdatePlayer =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ?? ?? ?? ?? 44 0F 29 94 24 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? "
    "44 0F 29 84 24 ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 4C 89 C6";
// renderLevel se encuentra por el lugar donde se la llama (E8 = "call").
constexpr const char* kRenderLevelCall = "E8 ?? ?? ?? ?? 31 FF 48 8B 86 ?? ?? ?? ?? 48 85 C0";

// En el componente de cámara, hacia dónde mira está guardado como un
// "cuaternión" (4 números que representan una rotación) en +0x30.
constexpr std::uintptr_t kCameraLookAngles = 0x30;
// El campo de visión vertical de la cámara, en radianes (en +0x4C está la
// proporción ancho/alto de la pantalla). Medido en 1.26.63: 1,15 rad con el
// FOV del juego en 60; el juego lo recalcula en cada frame.
constexpr std::uintptr_t kCameraFov = 0x50;

using GetGammaFn = float(__fastcall*)(void* options, void* a2);
using GetPerspectiveFn = int(__fastcall*)(void* options);
using UpdatePlayerFn = void(__fastcall*)(void* camera, void* a, void* b);
using RenderLevelFn = void(__fastcall*)(void* levelRenderer, void* screenContext, void* unk);

GetGammaFn g_getGamma = nullptr;
GetPerspectiveFn g_getPerspective = nullptr;
UpdatePlayerFn g_updatePlayer = nullptr;
RenderLevelFn g_renderLevel = nullptr;

std::atomic<bool> g_active{false};
std::atomic<bool> g_gammaOk{false};
std::atomic<bool> g_perspectiveOk{false};
std::atomic<bool> g_cameraOk{false};
std::atomic<float> g_gamma{-1.0f};
std::atomic<int> g_perspective{-1};
std::atomic<bool> g_freelook{false};
std::atomic<float> g_lockPitch{0.0f};
std::atomic<float> g_lockYaw{0.0f};
std::atomic<bool> g_loggedCamera{false};
std::atomic<bool> g_loggedRender{false};
std::atomic<std::uintptr_t> g_camera{0};       // el componente de cámara (lo vemos en _updatePlayer)
std::atomic<float> g_fovMultiplier{1.0f};
std::atomic<float> g_fovOverride{0.0f};        // grados (0 = sin cambio)
float g_lastWrittenFov = 0.0f;                 // lo que escribimos el frame anterior
float g_baseFov = 0.0f;                        // el FOV que calculó el juego
std::atomic<float> g_smoothing{12.0f};
float g_currentMultiplier = 1.0f;              // el multiplicador animado (hilo de render)
std::int64_t g_lastApply = 0;

// Cambia el FOV de la cámara (en el hilo de render, antes de dibujar).
void ApplyFov() {
    const std::uintptr_t cam = g_camera.load(std::memory_order_relaxed);
    // Animación: el multiplicador real se acerca al pedido de a poco.
    const float target = g_fovMultiplier.load(std::memory_order_relaxed);
    const std::int64_t now = perf::Now();
    const float dt = g_lastApply == 0 ? 0.0f : static_cast<float>(perf::TicksToMs(now - g_lastApply) / 1000.0);
    g_lastApply = now;
    const float speed = g_smoothing.load(std::memory_order_relaxed);
    if (speed <= 0.0f || dt <= 0.0f || dt > 0.5f) {
        g_currentMultiplier = target;
    } else {
        const float t = 1.0f - std::exp(-speed * dt);
        g_currentMultiplier += (target - g_currentMultiplier) * t;
        if (std::fabs(g_currentMultiplier - target) < 0.0005f) {
            g_currentMultiplier = target;
        }
    }
    const float multiplier = g_currentMultiplier;
    const float overrideDeg = g_fovOverride.load(std::memory_order_relaxed);
    if (cam == 0 || (multiplier == 1.0f && overrideDeg <= 0.0f)) {
        g_lastWrittenFov = 0.0f;
        return;
    }
    float current = 0.0f;
    if (!SafeRead(cam + kCameraFov, current) || !(current > 0.05f && current < 3.0f)) {
        return;
    }
    // Si el valor es el que escribimos nosotros, el juego no lo recalculó:
    // seguimos usando la base de antes (para no achicarlo una y otra vez).
    if (current != g_lastWrittenFov) {
        g_baseFov = current;
    }
    float fov = overrideDeg > 0.0f ? overrideDeg * 3.14159265f / 180.0f : g_baseFov;
    fov *= multiplier;
    fov = fov < 0.02f ? 0.02f : (fov > 2.8f ? 2.8f : fov);
    *reinterpret_cast<float*>(cam + kCameraFov) = fov;
    g_lastWrittenFov = fov;
}

// Rotación (pitch, yaw en grados) -> cuaternión, con el mismo orden de
// componentes que usa el juego en este campo.
void RotationToQuaternion(float pitchDeg, float yawDeg, float out[4]) {
    constexpr float kDegToRad = 3.14159265358979f / 180.0f;
    const float cx = std::cos(pitchDeg * kDegToRad * 0.5f);
    const float sx = std::sin(pitchDeg * kDegToRad * 0.5f);
    const float cy = std::cos(yawDeg * kDegToRad * 0.5f);
    const float sy = std::sin(yawDeg * kDegToRad * 0.5f);
    // (sin roll: cz = 1, sz = 0)
    out[0] = -sx * sy;
    out[1] = cx * cy;
    out[2] = sx * cy;
    out[3] = cx * sy;
}

float __fastcall GetGammaDetour(void* options, void* a2) {
    hooks::ActiveCallGuard guard;
    const float original = g_getGamma(options, a2);
    const float forced = g_gamma.load(std::memory_order_relaxed);
    return g_active.load(std::memory_order_relaxed) && forced >= 0.0f ? forced : original;
}

int __fastcall GetPerspectiveDetour(void* options) {
    hooks::ActiveCallGuard guard;
    const int original = g_getPerspective(options);
    const int forced = g_perspective.load(std::memory_order_relaxed);
    return g_active.load(std::memory_order_relaxed) && forced >= 0 ? forced : original;
}

void __fastcall UpdatePlayerDetour(void* camera, void* a, void* b) {
    hooks::ActiveCallGuard guard;
    const auto cam = reinterpret_cast<std::uintptr_t>(camera);
    g_camera.store(cam, std::memory_order_relaxed);
    if (!g_loggedCamera.exchange(true)) {
        // Diagnóstico (una vez): qué hay en el componente de cámara.
        float values[24]{};
        if (SafeRead(cam + 0x30, values)) {
            log::Info("SDK: cámara en 0x{:X}. +0x30: {:.3f} {:.3f} {:.3f} {:.3f} | +0x40: {:.2f} {:.2f} {:.2f} | "
                      "+0x4C: {:.4f} {:.4f} | +0x54: {:.3f} {:.3f}",
                      cam, values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                      values[8], values[9], values[10]);
        }
    }
    if (g_active.load(std::memory_order_relaxed) && g_freelook.load(std::memory_order_relaxed)) {
        // Le hacemos creer al juego que la cámara mira hacia la rotación
        // guardada (el jugador no gira) y después devolvemos la real.
        float saved[4]{};
        if (SafeRead(cam + kCameraLookAngles, saved)) {
            float locked[4];
            RotationToQuaternion(g_lockPitch.load(), g_lockYaw.load(), locked);
            std::memcpy(reinterpret_cast<void*>(cam + kCameraLookAngles), locked, sizeof(locked));
            g_updatePlayer(camera, a, b);
            std::memcpy(reinterpret_cast<void*>(cam + kCameraLookAngles), saved, sizeof(saved));
            return;
        }
    }
    g_updatePlayer(camera, a, b);
}

void __fastcall RenderLevelDetour(void* levelRenderer, void* screenContext, void* unk) {
    hooks::ActiveCallGuard guard;
    if (!g_loggedRender.exchange(true)) {
        log::Info("SDK: renderLevel: LevelRenderer 0x{:X}, ScreenContext 0x{:X}.",
                  reinterpret_cast<std::uintptr_t>(levelRenderer), reinterpret_cast<std::uintptr_t>(screenContext));
    }
    if (g_active.load(std::memory_order_relaxed)) {
        ApplyFov();  // zoom / FOV: justo antes de dibujar el mundo
    }
    g_renderLevel(levelRenderer, screenContext, unk);
}

// Busca la firma y hookea. "resolveCall": la firma apunta a un "call" y la
// función es su destino.
template <typename Fn>
bool HookSignature(const char* pattern, void* detour, Fn& original, const char* name, bool resolveCall = false) {
    const ScanResult match = FindPattern(pattern);
    if (match.matches != 1) {
        log::Warn("SDK: firma de {} no encontrada ({} coincidencias).", name, match.matches);
        return false;
    }
    std::uintptr_t target = match.address;
    if (resolveCall) {
        target = ResolveRipRelative(match.address + 1, match.address + 5);
    }
    void* address = reinterpret_cast<void*>(target);
    if (!hooks::Create(address, detour, reinterpret_cast<void**>(&original), name) || !hooks::Enable(address, name)) {
        return false;
    }
    log::Info("SDK: {} en juego+0x{:X}.", name, target - GameBase());
    return true;
}

}  // namespace

bool Init() {
    g_gammaOk = HookSignature(kGetGamma, reinterpret_cast<void*>(&GetGammaDetour), g_getGamma, "juego: brillo (getGamma)");
    g_perspectiveOk = HookSignature(kGetPerspective, reinterpret_cast<void*>(&GetPerspectiveDetour), g_getPerspective,
                                    "juego: perspectiva (getPerspective)");
    g_cameraOk = HookSignature(kUpdatePlayer, reinterpret_cast<void*>(&UpdatePlayerDetour), g_updatePlayer,
                               "juego: cámara -> jugador (_updatePlayer)");
    HookSignature(kRenderLevelCall, reinterpret_cast<void*>(&RenderLevelDetour), g_renderLevel,
                  "juego: dibujar el mundo (renderLevel)", true);
    g_active.store(true);
    return g_gammaOk || g_perspectiveOk || g_cameraOk;
}

bool GammaAvailable() {
    return g_gammaOk.load();
}

bool PerspectiveAvailable() {
    return g_perspectiveOk.load();
}

bool CameraAvailable() {
    return g_cameraOk.load();
}

void SetGammaOverride(float gamma) {
    g_gamma.store(gamma);
}

void SetPerspectiveOverride(int perspective) {
    g_perspective.store(perspective);
}

void SetFovMultiplier(float multiplier) {
    g_fovMultiplier.store(multiplier);
}

void SetFovOverride(float degrees) {
    g_fovOverride.store(degrees);
}

void SetZoomSmoothing(float speed) {
    g_smoothing.store(speed);
}

void SetFreelook(bool active, float pitch, float yaw) {
    g_lockPitch.store(pitch);
    g_lockYaw.store(yaw);
    g_freelook.store(active);
}

void BeginUnload() {
    g_active.store(false);
}

}  // namespace luma::sdk::camera
