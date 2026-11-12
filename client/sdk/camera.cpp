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
#include "sdk/draw3d.h"
#include "sdk/dynlight.h"
#include "sdk/memory.h"
#include "sdk/scanner.h"
#include "sdk/game_options.h"

namespace luma::sdk::camera {

namespace {

// --- Firmas (1.26.6x) ----------------------------------------------------------------
// getGamma y getPerspective son casi iguales: leen una opción por su número
// (0x32 = gamma, 0x03 = perspectiva). Ese número al final las distingue.
constexpr const char* kGetGamma =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B 01 48 8B 40 08 48 8D 54 24 ?? 41 B8 32 00 00 00";
constexpr const char* kGetPerspective =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B 01 48 8B 40 08 48 8D 54 24 ?? 41 B8 03 00 00 00";
// La misma familia, opción 0x1A6 = "ocultar la mano".
constexpr const char* kGetHideHand =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 31 E0 48 89 44 24 ?? 48 8B 01 48 8B 40 08 48 8D 54 24 ?? 41 B8 A6 01 00 00";
constexpr const char* kUpdatePlayer =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ?? ?? ?? ?? 44 0F 29 94 24 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? "
    "44 0F 29 84 24 ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 4C 89 C6";
// renderLevel se encuentra por el lugar donde se la llama (E8 = "call").
constexpr const char* kRenderLevelCall = "E8 ?? ?? ?? ?? 31 FF 48 8B 86 ?? ?? ?? ?? 48 85 C0";

// En el componente de cámara, hacia dónde mira está guardado como un
// "cuaternión" (4 números que representan una rotación) en +0x30.
constexpr std::uintptr_t kCameraLookAngles = 0x30;


using GetGammaFn = float(__fastcall*)(void* options, void* a2);
using GetPerspectiveFn = int(__fastcall*)(void* options);
using GetHideHandFn = bool(__fastcall*)(void* options);
using UpdatePlayerFn = void(__fastcall*)(void* camera, void* a, void* b);
using RenderLevelFn = void(__fastcall*)(void* levelRenderer, void* screenContext, void* unk);

GetGammaFn g_getGamma = nullptr;
GetPerspectiveFn g_getPerspective = nullptr;
GetHideHandFn g_getHideHand = nullptr;
std::atomic<bool> g_hideHandOk{false};
std::atomic<bool> g_hideHand{false};
std::atomic<float> g_publishedMultiplier{1.0f};
UpdatePlayerFn g_updatePlayer = nullptr;
RenderLevelFn g_renderLevel = nullptr;

std::atomic<bool> g_active{false};
std::atomic<bool> g_gammaOk{false};
std::atomic<bool> g_perspectiveOk{false};
std::atomic<bool> g_cameraOk{false};
std::atomic<bool> g_renderOk{false};
std::atomic<float> g_gamma{-1.0f};
std::atomic<int> g_perspective{-1};
std::atomic<int> g_lastPerspective{-1};  // what the game answered last (CurrentPerspective)
std::atomic<bool> g_freelook{false};
std::atomic<float> g_lockPitch{0.0f};
std::atomic<float> g_lockYaw{0.0f};
std::atomic<bool> g_loggedCamera{false};
std::atomic<bool> g_loggedRender{false};
std::atomic<std::uintptr_t> g_camera{0};       // el componente de cámara (lo vemos en _updatePlayer)
std::atomic<std::uintptr_t> g_levelRenderer{0};  // the last LevelRenderer that drew the world (GetView)
std::atomic<float> g_fovMultiplier{1.0f};
std::atomic<float> g_fovOverride{0.0f};        // grados (0 = sin cambio)
std::atomic<float> g_smoothing{12.0f};
float g_currentMultiplier = 1.0f;              // el multiplicador animado (hilo de render)
std::int64_t g_lastApply = 0;

// --- Zoom y FOV ------------------------------------------------------------------
// El FOV con el que se DIBUJA está en el "LevelRendererPlayer" (lo que dibuja
// el mundo desde los ojos del jugador): LevelRenderer +0x480 -> +0xF50 (escala
// horizontal) y +0xF64 (escala vertical). Son los números de la matriz de
// proyección: más grandes = más zoom. (Los mismos offsets que usa Latite para
// 1.26.6x; los confirmamos leyendo la memoria: su proporción es la de la
// pantalla, 9/16.)
//
// NO es el FOV del componente de cámara (+0x50): ese lo probamos y el juego
// no lo usa para dibujar.
constexpr std::uintptr_t kLevelRendererPlayer = 0x480;
constexpr std::uintptr_t kFovScaleX = 0xF50;
constexpr std::uintptr_t kFovScaleY = 0xF64;

float g_lastWritten[2] = {0.0f, 0.0f};  // lo que escribimos (para no acumular)
float g_base[2] = {0.0f, 0.0f};         // lo que calculó el juego

// Avanza la animación del zoom (una vez por frame).
void StepZoomAnimation() {
    const float target = g_fovMultiplier.load(std::memory_order_relaxed);
    const std::int64_t now = perf::Now();
    const float dt = g_lastApply == 0 ? 0.0f : static_cast<float>(perf::TicksToMs(now - g_lastApply) / 1000.0);
    g_lastApply = now;
    const float speed = g_smoothing.load(std::memory_order_relaxed);
    if (speed <= 0.0f || dt <= 0.0f || dt > 0.5f) {
        g_currentMultiplier = target;
    } else {
        g_currentMultiplier += (target - g_currentMultiplier) * (1.0f - std::exp(-speed * dt));
        if (std::fabs(g_currentMultiplier - target) < 0.0005f) {
            g_currentMultiplier = target;
        }
    }
    g_publishedMultiplier.store(g_currentMultiplier, std::memory_order_relaxed);
}

// Escribe la escala del FOV en el LevelRendererPlayer.
void ApplyFov(std::uintptr_t levelRenderer) {
    const float multiplier = g_currentMultiplier;
    const float overrideDeg = g_fovOverride.load(std::memory_order_relaxed);
    if (multiplier == 1.0f && overrideDeg <= 0.0f) {
        g_lastWritten[0] = g_lastWritten[1] = 0.0f;
        return;
    }
    std::uintptr_t lrp = 0;
    float scale[2]{};
    if (!SafeRead(levelRenderer + kLevelRendererPlayer, lrp) || !LooksLikePointer(lrp) ||
        !SafeRead(lrp + kFovScaleX, scale[0]) || !SafeRead(lrp + kFovScaleY, scale[1]) ||
        !(scale[0] > 0.01f && scale[0] < 50.0f && scale[1] > 0.01f && scale[1] < 50.0f)) {
        return;
    }
    // Si siguen siendo los que escribimos, el juego no los recalculó todavía:
    // usar la base de antes (si no, el zoom se multiplicaría frame a frame).
    for (int i = 0; i < 2; ++i) {
        if (scale[i] != g_lastWritten[i]) {
            g_base[i] = scale[i];
        }
    }
    float out[2] = {g_base[0], g_base[1]};
    if (overrideDeg > 0.0f) {
        // FOV fijo: escala vertical = 1 / tan(FOV / 2); la horizontal mantiene la proporción.
        const float ratio = g_base[0] / g_base[1];
        out[1] = 1.0f / std::tan(overrideDeg * 3.14159265f / 360.0f);
        out[0] = out[1] * ratio;
    }
    // Zoom: el multiplicador achica el FOV, o sea, agranda la escala.
    out[0] /= multiplier;
    out[1] /= multiplier;
    *reinterpret_cast<float*>(lrp + kFovScaleX) = out[0];
    *reinterpret_cast<float*>(lrp + kFovScaleY) = out[1];
    g_lastWritten[0] = out[0];
    g_lastWritten[1] = out[1];
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

bool __fastcall GetHideHandDetour(void* options) {
    hooks::ActiveCallGuard guard;
    const bool original = g_getHideHand(options);
    return (g_active.load(std::memory_order_relaxed) && g_hideHand.load(std::memory_order_relaxed)) || original;
}

int __fastcall GetPerspectiveDetour(void* options) {
    hooks::ActiveCallGuard guard;
    gameoptions::OnOptions(options);  // (once: finds "Hide HUD", sdk/game_options.h)
    const int original = g_getPerspective(options);
    g_lastPerspective.store(original, std::memory_order_relaxed);
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
    const bool active = g_active.load(std::memory_order_relaxed);
    const auto lr = reinterpret_cast<std::uintptr_t>(levelRenderer);
    g_levelRenderer.store(lr, std::memory_order_relaxed);
    if (active) {
        // Zoom / FOV: antes de dibujar (por si el juego lo usa en este frame)
        // y después (así lo hace Latite: queda para el frame siguiente).
        StepZoomAnimation();
        ApplyFov(lr);
    }
    draw3d::BeginLevel(lr);
    dynlight::EnterRenderLevel(lr);  // dynamic lights: rebuilds + light for entities (sdk/dynlight.h)
    g_renderLevel(levelRenderer, screenContext, unk);
    dynlight::LeaveRenderLevel();
    draw3d::EndLevel();
    if (active) {
        ApplyFov(lr);
        // Hitboxes y otros dibujos dentro del mundo (sdk/draw3d.h).
        draw3d::OnRenderLevel(lr, reinterpret_cast<std::uintptr_t>(screenContext));
    }
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

bool GetView(View& out) {
    const std::uintptr_t lr = g_levelRenderer.load(std::memory_order_relaxed);
    const std::uintptr_t cam = g_camera.load(std::memory_order_relaxed);
    if (lr == 0 || cam == 0) return false;
    std::uintptr_t lrp = 0;
    float q[4]{};
    if (!SafeRead(lr + kLevelRendererPlayer, lrp) || !LooksLikePointer(lrp) || !SafeRead(lrp + 0x658, out.pos) ||
        !SafeRead(lrp + kFovScaleX, out.scaleX) || !SafeRead(lrp + kFovScaleY, out.scaleY) ||
        !SafeRead(cam + kCameraLookAngles, q)) {
        return false;
    }
    for (float v : {out.pos[0], out.pos[1], out.pos[2], q[0], q[1], q[2], q[3]}) {
        if (!std::isfinite(v)) return false;
    }
    if (!(out.scaleX > 0.01f && out.scaleX < 50.0f && out.scaleY > 0.01f && out.scaleY < 50.0f)) return false;
    // The inverse of RotationToQuaternion: w = q[1] = cos(p/2)cos(y/2),
    // x = q[2] = sin(p/2)cos(y/2), y = q[3] = cos(p/2)sin(y/2).
    constexpr float kRadToDeg = 180.0f / 3.14159265358979f;
    auto wrap = [](float deg) {
        deg = std::fmod(deg + 180.0f, 360.0f);
        if (deg < 0.0f) deg += 360.0f;
        return deg - 180.0f;
    };
    out.pitch = wrap(2.0f * std::atan2(q[2], q[1]) * kRadToDeg);
    out.yaw = wrap(2.0f * std::atan2(q[3], q[1]) * kRadToDeg);
    return std::fabs(out.pitch) <= 90.5f;
}

bool Init() {
    g_gammaOk = HookSignature(kGetGamma, reinterpret_cast<void*>(&GetGammaDetour), g_getGamma, "juego: brillo (getGamma)");
    g_perspectiveOk = HookSignature(kGetPerspective, reinterpret_cast<void*>(&GetPerspectiveDetour), g_getPerspective,
                                    "juego: perspectiva (getPerspective)");
    g_hideHandOk = HookSignature(kGetHideHand, reinterpret_cast<void*>(&GetHideHandDetour), g_getHideHand,
                                 "juego: ocultar la mano (getHideHand)");
    g_cameraOk = HookSignature(kUpdatePlayer, reinterpret_cast<void*>(&UpdatePlayerDetour), g_updatePlayer,
                               "juego: cámara -> jugador (_updatePlayer)");
    g_renderOk = HookSignature(kRenderLevelCall, reinterpret_cast<void*>(&RenderLevelDetour), g_renderLevel,
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

bool RenderAvailable() {
    return g_renderOk.load();
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

float CurrentFovMultiplier() {
    return g_publishedMultiplier.load(std::memory_order_relaxed);
}

void SetHideHand(bool hide) {
    g_hideHand.store(hide);
}

bool HideHandAvailable() {
    return g_hideHandOk.load();
}

void SetZoomSmoothing(float speed) {
    g_smoothing.store(speed);
}

bool FreelookActive() {
    return g_active.load(std::memory_order_relaxed) && g_freelook.load(std::memory_order_relaxed);
}

int CurrentPerspective() {
    const int forced = g_perspective.load(std::memory_order_relaxed);
    if (g_active.load(std::memory_order_relaxed) && forced >= 0) return forced;
    return g_lastPerspective.load(std::memory_order_relaxed);
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
