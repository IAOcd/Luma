// =============================================================================
//  player.cpp - Captura y lectura del jugador local (ver player.h)
// =============================================================================
#include "sdk/player.h"

#include <atomic>
#include <cmath>

#include "core/hooks.h"
#include "core/logger.h"
#include "render/notifications.h"
#include "sdk/camera.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/scanner.h"

namespace luma::sdk::player {

namespace {

// Inicio de LocalPlayer::applyTurnDelta en 1.26.6x: guarda registros, reserva
// pila, guarda xmm6-xmm10 (trabaja con floats: los ángulos) y copia el
// segundo parámetro (el giro) a r12.
constexpr const char* kApplyTurnDelta =
    "55 41 57 41 56 41 54 56 57 53 48 81 EC ?? ?? ?? ?? 48 8D AC 24 ?? ?? ?? ?? 44 0F 29 55 ?? "
    "44 0F 29 4D ?? 44 0F 29 45 ?? 0F 29 7D ?? 0F 29 75 ?? 48 C7 45 ?? ?? ?? ?? ?? 49 89 D4";

constexpr float kEyeHeight = 1.62f;

using ApplyTurnDeltaFn = void(__fastcall*)(void* self, const float* delta);
ApplyTurnDeltaFn g_original = nullptr;

std::atomic<std::uintptr_t> g_player{0};
std::atomic<std::uintptr_t> g_vtable{0};
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_initialized{false};  // ¿ya se intentó Init?
std::atomic<bool> g_offsetsBad{false};   // la comprobación de offsets falló
std::atomic<bool> g_validated{false};
std::atomic<float> g_sensitivity{1.0f};
std::atomic<float> g_zoomSensitivity{1.0f};
std::atomic<bool> g_followZoom{false};
std::atomic<float> g_followStrength{1.0f};

// Lee un puntero del jugador a uno de sus componentes.
bool Component(std::uintptr_t player, std::uintptr_t offset, std::uintptr_t& out) {
    return SafeRead(player + offset, out) && LooksLikePointer(out);
}

// ¿Los offsets tienen sentido para ESTE objeto? La caja de un jugador mide
// 0,6 de ancho y 1,8 de alto (1,5 agachado; menos si nada o planea).
bool Validate(std::uintptr_t player) {
    std::uintptr_t aabb = 0;
    float size[2]{};
    if (!Component(player, offsets::kActorAabb, aabb) || !SafeRead(aabb + offsets::kAabbSize, size)) {
        return false;
    }
    return std::fabs(size[0] - 0.6f) < 0.05f && size[1] > 0.5f && size[1] < 2.0f;
}

void __fastcall ApplyTurnDeltaDetour(void* self, const float* delta) {
    hooks::ActiveCallGuard guard;
    if (!g_hooked.load(std::memory_order_relaxed)) {
        g_original(self, delta);
        return;
    }
    const auto value = reinterpret_cast<std::uintptr_t>(self);
    if (g_player.load(std::memory_order_relaxed) != value) {
        std::uintptr_t vtable = 0;
        SafeRead(value, vtable);
        g_vtable.store(vtable);
        g_player.store(value);
        log::Info("SDK: jugador local en 0x{:X}.", value);
        if (!g_validated.exchange(true)) {
            if (Validate(value)) {
                log::Info("SDK: offsets del jugador comprobados (caja de 0,6 x 1,8).");
            } else {
                g_offsetsBad.store(true);
                log::Warn("SDK: los offsets del jugador no coinciden con esta versión del juego.");
                notify::Push(notify::Kind::Warning, "Datos del jugador no compatibles: módulos desactivados");
            }
        }
    }
    // Sensibilidad: multiplicar el giro que el juego va a aplicar.
    float sensitivity = g_sensitivity.load(std::memory_order_relaxed) * g_zoomSensitivity.load(std::memory_order_relaxed);
    if (g_followZoom.load(std::memory_order_relaxed)) {
        const float m = sdk::camera::CurrentFovMultiplier();
        if (m < 1.0f) {
            sensitivity *= std::pow(m, g_followStrength.load(std::memory_order_relaxed));
        }
    }
    if (sensitivity != 1.0f && delta != nullptr) {
        const float scaled[2] = {delta[0] * sensitivity, delta[1] * sensitivity};
        g_original(self, scaled);
        return;
    }
    g_original(self, delta);
}

}  // namespace

bool Init() {
    g_initialized.store(true);
    const ScanResult match = FindPattern(kApplyTurnDelta);
    if (match.matches != 1) {
        log::Warn("SDK: firma del jugador local no encontrada ({} coincidencias).", match.matches);
        notify::Push(notify::Kind::Warning, "Firma no encontrada: módulos del jugador desactivados");
        return false;
    }
    void* target = reinterpret_cast<void*>(match.address);
    if (!hooks::Create(target, reinterpret_cast<void*>(&ApplyTurnDeltaDetour), reinterpret_cast<void**>(&g_original),
                       "juego: girar la cámara del jugador") ||
        !hooks::Enable(target, "juego: girar la cámara del jugador")) {
        return false;
    }
    g_hooked.store(true);
    log::Info("SDK: girar la cámara del jugador en juego+0x{:X}.", match.address - GameBase());
    return true;
}

bool Initialized() {
    return g_initialized.load();
}

bool Available() {
    return g_hooked.load(std::memory_order_relaxed) && !g_offsetsBad.load(std::memory_order_relaxed);
}

std::uintptr_t LocalPlayer() {
    return g_player.load(std::memory_order_relaxed);
}

bool Read(Snapshot& out) {
    if (!Available()) {
        return false;
    }
    const std::uintptr_t player = g_player.load(std::memory_order_relaxed);
    std::uintptr_t vtable = 0;
    if (player == 0 || !SafeRead(player, vtable) || vtable != g_vtable.load(std::memory_order_relaxed)) {
        return false;  // no hay jugador, o esa memoria ya es otra cosa
    }
    std::uintptr_t state = 0;
    std::uintptr_t aabb = 0;
    std::uintptr_t rotation = 0;
    float rot[2]{};
    if (!Component(player, offsets::kActorStateVector, state) || !Component(player, offsets::kActorAabb, aabb) ||
        !Component(player, offsets::kActorRotation, rotation) || !SafeRead(state + offsets::kStatePos, out.eye) || !SafeRead(state + offsets::kStatePosOld, out.eyeOld) ||
        !SafeRead(state + offsets::kStateVelocity, out.velocity) || !SafeRead(aabb + offsets::kAabbMin, out.feet) ||
        !SafeRead(rotation + offsets::kRotPitch, rot)) {
        return false;
    }
    // Los pies: x y z del centro (como los muestra el juego), y la altura
    // de la parte de abajo de la caja.
    out.feet[0] = out.eye[0];
    out.feet[2] = out.eye[2];
    if (!std::isfinite(out.feet[1])) {
        out.feet[1] = out.eye[1] - kEyeHeight;
    }
    out.pitch = rot[0];
    out.yaw = rot[1];
    SafeRead(player + offsets::kActorTicksExisted, out.ticks);
    return std::isfinite(out.eye[0]) && std::isfinite(out.eye[1]) && std::isfinite(out.eye[2]);
}

void SetZoomSensitivityFollow(bool follow, float strength) {
    g_followStrength.store(strength);
    g_followZoom.store(follow);
}

void SetZoomSensitivity(float multiplier) {
    g_zoomSensitivity.store(multiplier);
}

void SetSensitivity(float multiplier) {
    g_sensitivity.store(multiplier);
}

void BeginUnload() {
    g_hooked.store(false);
}

}  // namespace luma::sdk::player
