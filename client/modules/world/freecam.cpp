// =============================================================================
//  freecam.cpp - Free cam (ver freecam.h)
// =============================================================================
#include "modules/world/freecam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <windows.h>

#include "modules/module_manager.h"
#include "render/hud.h"
#include "render/icons.h"
#include "render/input.h"
#include "render/notifications.h"
#include "render/overlay.h"
#include "render/ui.h"
#include "sdk/camera.h"
#include "sdk/mouse_grab.h"
#include "sdk/mouse_queue.h"
#include "sdk/player.h"

namespace luma::modules::world {

namespace ui = render::ui;

FreeCam::FreeCam()
    : Module("freecam", "Free cam",
             "Fly the camera around while your player stays still. Only in your own worlds.", Category::World) {
    localOnly_ = true;  // en un servidor no se activa (y se apaga solo al entrar a uno)
    SetDefaultKeybind(VK_F6);
}

const char* FreeCam::IconSvg() const {
    return render::icons::kMove;
}

bool FreeCam::SdkReady() const {
    // _updatePlayer (que el jugador no gire), renderLevel (mover la cámara
    // mientras se dibuja) y getPerspective (ver a tu jugador).
    return sdk::camera::CameraAvailable() && sdk::camera::RenderAvailable() && sdk::camera::PerspectiveAvailable();
}

void FreeCam::ApplyInput() const {
    sdk::camera::FreecamInput input;
    input.speed = speed_;
    input.smoothing = smoothing_;
    input.fullLook = fullLook_;
    sdk::camera::SetFreecamInput(input);
}

void FreeCam::OnEnable() {
    // Hacia dónde mira el jugador ahora: se queda así mientras vuelas.
    sdk::player::Snapshot p;
    if (!sdk::camera::HasCamera() || !sdk::player::Read(p)) {
        // Sin mundo (por ejemplo, en el menú principal) no hay nada que mover.
        failed_ = true;
        return;
    }
    failed_ = false;
    // El freelook usa el mismo truco: los dos a la vez se pisarían.
    if (Module* freelook = Manager::Find("freelook"); freelook != nullptr && freelook->IsEnabled()) {
        Manager::SetEnabled(*freelook, false, false);
    }
    ApplyInput();
    sdk::camera::SetFreecam(true, p.pitch, p.yaw);
    if (showBody_) {
        sdk::camera::SetPerspectiveOverride(1);  // tercera persona: el juego dibuja tu cuerpo
    }
    sdk::mousequeue::SetBlockClicks(true);
    zoom_ = Manager::Find("zoom");
    zoomHadWheel_ = zoom_ != nullptr && zoom_->IsEnabled();
    if (!zoomHadWheel_) {
        render::input::SetWheelCapture(true);
        render::input::TakeWheelSteps();  // descartar giros viejos
    }
    shownUntil_ = GetTickCount64() + 2500;  // mostrar la velocidad y los controles un momento
    playing_ = !render::overlay::IsMenuOpen() && sdk::mouse::GameMouseState() != 0;
    render::input::SetFreecamKeys(playing_);
    sdk::camera::SetFreecamAllowMove(playing_);
}

void FreeCam::OnDisable() {
    sdk::camera::SetFreecamAllowMove(false);
    sdk::camera::SetFreecam(false, 0.0f, 0.0f);
    sdk::camera::SetPerspectiveOverride(-1);
    sdk::mousequeue::SetBlockClicks(false);
    render::input::SetWheelCapture(false);
    render::input::SetFreecamKeys(false);
    playing_ = false;
}

void FreeCam::OnTick() {
    if (failed_) {
        failed_ = false;
        Manager::SetEnabled(*this, false, false);
        notify::Push(notify::Kind::Warning, Tr("Free cam: enter a world first"));
        return;
    }
    // Saliste del mundo: apagarlo (la cámara de ese mundo ya no existe).
    sdk::player::Snapshot p;
    if (!sdk::player::Read(p)) {
        Manager::SetEnabled(*this, false, false);
        return;
    }
    // Modo "mantener": al soltar la tecla, se desactiva.
    if (mode_ == 0 && Keybind() != keys::kNone && (GetAsyncKeyState(Keybind()) & 0x8000) == 0) {
        Manager::SetEnabled(*this, false, false);
        return;
    }
    // Solo jugando: en el chat, un cartel o el inventario, las teclas son
    // para el juego (si no, no podrías escribir una "w").
    const bool playing = !render::overlay::IsMenuOpen() && sdk::mouse::GameMouseState() != 0;
    if (playing != playing_) {
        playing_ = playing;
        render::input::SetFreecamKeys(playing);
        sdk::camera::SetFreecamAllowMove(playing);
    }
    // Rueda: más o menos velocidad. Pero si el zoom está activo, la rueda es
    // suya (acercar y alejar): los dos leen los mismos giros y el free cam,
    // que va primero, se los quedaba todos.
    const bool zoomActive = zoom_ != nullptr && zoom_->IsEnabled();
    if (zoomActive) {
        zoomHadWheel_ = true;
        return;
    }
    if (zoomHadWheel_) {
        // El zoom terminó y soltó la rueda: volver a tomarla, sin los giros viejos.
        zoomHadWheel_ = false;
        render::input::SetWheelCapture(true);
        render::input::TakeWheelSteps();
        return;
    }
    const int steps = render::input::TakeWheelSteps();
    if (steps != 0) {
        speed_ = std::clamp(speed_ * std::pow(1.25f, static_cast<float>(steps)), 1.0f, 100.0f);
        ApplyInput();
        shownUntil_ = GetTickCount64() + 1500;
    }
}

// Un cartelito arriba al centro: la velocidad (al activarlo y al cambiarla
// con la rueda), y se desvanece solo. Sin texto nuevo por frame: se arma en
// un búfer fijo (nada de memoria nueva en el hot path).
ImVec2 FreeCam::OnRenderHud(const HudFrame& frame) {
    const std::uint64_t now = GetTickCount64();
    if (now >= shownUntil_ || render::overlay::IsMenuOpen()) {
        return ImVec2(0, 0);
    }
    const float left = static_cast<float>(shownUntil_ - now);
    const float alpha = std::clamp(left / 400.0f, 0.0f, 1.0f);  // los últimos 0,4 s se desvanece
    char text[96];
    std::snprintf(text, sizeof(text), "%s  %.0f %s", Tr("Free cam"), speed_, Tr("blocks/s"));
    const render::hud::Metrics m = render::hud::Measure(1.0f);
    const ImVec2 size = render::hud::TextSize(m, text);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 pos(std::floor((display.x - size.x) * 0.5f), std::floor(display.y * 0.08f));
    render::hud::Text(frame.draw, m, pos, text, render::hud::TextColor(alpha));
    return ImVec2(0, 0);
}

bool FreeCam::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Key", &mode_, {"Hold", "Toggle"});
    changed |= ui::Slider("Speed", &speed_, 1.0f, 100.0f, "%.0f");
    ui::Hint("Blocks per second. The mouse wheel changes it while flying; hold Ctrl to go 3x faster.");
    changed |= ui::Slider("Smoothness", &smoothing_, 0.0f, 20.0f, "%.0f");
    ui::Hint("How fast it speeds up and slows down. 0 = instant; low numbers glide more.");
    changed |= ui::Toggle("Fly where you look", &fullLook_, "(W also goes up and down)");
    changed |= ui::Toggle("Show my player", &showBody_, "(third person)");

    ui::Hint("WASD: move. Space: up. Shift: down. Only in your own worlds.");
    if (changed && IsEnabled()) {
        ApplyInput();
        sdk::camera::SetPerspectiveOverride(showBody_ ? 1 : -1);
    }
    return changed;
}

void FreeCam::SaveSettings(nlohmann::json& out) const {
    out["mode"] = mode_;
    out["speed"] = speed_;
    out["smoothing"] = smoothing_;
    out["fullLook"] = fullLook_;
    out["showBody"] = showBody_;
}

void FreeCam::LoadSettings(const nlohmann::json& in) {
    mode_ = std::clamp(in.value("mode", 1), 0, 1);
    speed_ = std::clamp(in.value("speed", 10.0f), 1.0f, 100.0f);
    smoothing_ = std::clamp(in.value("smoothing", 6.0f), 0.0f, 20.0f);
    fullLook_ = in.value("fullLook", true);
    showBody_ = in.value("showBody", true);
}

}  // namespace luma::modules::world
