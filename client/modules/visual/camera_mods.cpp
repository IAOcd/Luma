// =============================================================================
//  camera_mods.cpp - Implementación de Fullbright y Freelook
// =============================================================================
#include "modules/visual/camera_mods.h"

#include "render/icons.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

#include "render/cinematic.h"
#include "render/input.h"
#include "render/sound.h"
#include "render/ui.h"
#include "sdk/camera.h"
#include "modules/module_manager.h"
#include "sdk/player.h"

namespace luma::modules::visual {

namespace ui = render::ui;

// =============================================================================
//  Fullbright
// =============================================================================

Fullbright::Fullbright()
    : Module("fullbright", "Fullbright", "See everything lit up, even at night or in a cave.",
             Category::Visual) {}

const char* Fullbright::IconSvg() const {
    return render::icons::kSun;
}

bool Fullbright::SdkReady() const {
    return sdk::camera::GammaAvailable();
}

void Fullbright::OnEnable() {
    sdk::camera::SetGammaOverride(gamma_);
}

void Fullbright::OnDisable() {
    sdk::camera::SetGammaOverride(-1.0f);
}

bool Fullbright::DrawSettings() {
    const bool changed = ui::Slider("Brightness", &gamma_, 1.0f, 25.0f, "%.0f");
    if (changed && IsEnabled()) {
        sdk::camera::SetGammaOverride(gamma_);
    }
    ui::Hint("The game's maximum brightness is 1. At 10 or more, caves look like daytime.");
    return changed;
}

void Fullbright::SaveSettings(nlohmann::json& out) const {
    out["gamma"] = gamma_;
}

void Fullbright::LoadSettings(const nlohmann::json& in) {
    gamma_ = std::clamp(in.value("gamma", 10.0f), 1.0f, 25.0f);
    if (IsEnabled()) {
        sdk::camera::SetGammaOverride(gamma_);
    }
}

// =============================================================================
//  Freelook
// =============================================================================

Freelook::Freelook()
    : Module("freelook", "Freelook", "Look around in third person without turning your player. Give it a key.",
             Category::Visual) {}

const char* Freelook::IconSvg() const {
    return render::icons::kScan;
}

bool Freelook::SdkReady() const {
    return sdk::camera::CameraAvailable() && sdk::camera::PerspectiveAvailable();
}

void Freelook::OnEnable() {
    // Guardar hacia dónde mira el jugador ahora: se queda así mientras miras alrededor.
    sdk::player::Snapshot p;
    if (sdk::player::Read(p)) {
        sdk::camera::SetFreelook(true, p.pitch, p.yaw);
    }
    sdk::camera::SetPerspectiveOverride(view_);
}

void Freelook::OnDisable() {
    sdk::camera::SetFreelook(false, 0.0f, 0.0f);
    sdk::camera::SetPerspectiveOverride(-1);
}

void Freelook::OnTick() {
    // Modo "mantener": al soltar la tecla, se desactiva.
    if (mode_ == 0 && Keybind() != keys::kNone && (GetAsyncKeyState(Keybind()) & 0x8000) == 0) {
        Manager::SetEnabled(*this, false, false);
    }
}

bool Freelook::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Key", &mode_, {"Hold", "Toggle"});
    int index = view_ - 1;
    if (ui::Choice("Camera", &index, {"Behind", "In front"})) {
        view_ = index + 1;
        changed = true;
        if (IsEnabled()) {
            sdk::camera::SetPerspectiveOverride(view_);
        }
    }
    ui::Hint(mode_ == 0 ? "Hold its key to look around (set one in General)."
                        : "Press its key to turn it on and again to turn it off.");
    return changed;
}

void Freelook::SaveSettings(nlohmann::json& out) const {
    out["view"] = view_;
    out["mode"] = mode_;
}

void Freelook::LoadSettings(const nlohmann::json& in) {
    view_ = std::clamp(in.value("view", 1), 1, 2);
    mode_ = std::clamp(in.value("mode", 0), 0, 1);
}

// =============================================================================
//  Zoom
// =============================================================================
//  Tres estilos (cada uno es un punto de partida; todo se puede cambiar):
//    - Normal: zoom suave, nada más.
//    - Cinematográfico: barras negras de película, sin HUD, sin la mano y
//      una animación más lenta, como una toma de cine.
//    - Catalejo: un círculo en el centro y lo demás oscuro.

Zoom::Zoom()
    : Module("zoom", "Zoom",
             "Acerca la vista como un catalejo. Mantén su tecla (C) y usa la rueda para acercar más. Con estilos "
             "Normal, Cinematográfico y Catalejo.",
             Category::Visual) {
    SetDefaultKeybind('C');
}

const char* Zoom::IconSvg() const {
    return render::icons::kZoomIn;
}

bool Zoom::SdkReady() const {
    return sdk::camera::RenderAvailable();
}

void Zoom::ApplyStyle(int style) {
    style_ = style;
    bars_ = style == 1;
    spyglass_ = style == 2;
    hideHud_ = style != 0;
    hideHand_ = style != 0;
    hideGameUi_ = false;  // lo activa el usuario si quiere (pulsa F1 por él)
    smoothSpeed_ = style == 1 ? 5.0f : 12.0f;
}

void Zoom::Apply() const {
    sdk::camera::SetZoomSmoothing(smooth_ ? smoothSpeed_ : 0.0f);
    sdk::camera::SetFovMultiplier(IsEnabled() ? 1.0f / current_ : 1.0f);
    // La sensibilidad sigue al zoom animado (sdk/player): suave y en el mismo momento.
    sdk::player::SetZoomSensitivityFollow(followSensitivity_, followStrength_);
}

void Zoom::OnEnable() {
    if (!keepWheel_) {
        current_ = factor_;
    }
    render::input::SetWheelCapture(wheel_);
    render::input::TakeWheelSteps();  // descartar giros viejos
    Apply();
    render::cinematic::SetBars(bars_ ? barsHeight_ : 0.0f);
    render::cinematic::SetSpyglass(spyglass_);
    render::cinematic::SetHideHud(hideHud_);
    sdk::camera::SetHideHand(hideHand_);
    tappedF1_ = hideGameUi_;
    if (tappedF1_) {
        render::input::TapKey(VK_F1);  // el juego esconde su interfaz con F1
    }
    if (sounds_) {
        sound::Play(sound::Effect::ZoomIn);
    }
}

void Zoom::OnDisable() {
    render::input::SetWheelCapture(false);
    sdk::camera::SetFovMultiplier(1.0f);
    render::cinematic::SetBars(0.0f);
    render::cinematic::SetSpyglass(false);
    render::cinematic::SetHideHud(false);
    sdk::camera::SetHideHand(false);
    if (tappedF1_) {
        render::input::TapKey(VK_F1);  // devolver la interfaz del juego
        tappedF1_ = false;
    }
    if (sounds_) {
        sound::Play(sound::Effect::ZoomOut);
    }
}

void Zoom::OnTick() {
    if (wheel_) {
        const int steps = render::input::TakeWheelSteps();
        if (steps != 0) {
            const float before = current_;
            current_ = std::clamp(current_ * std::pow(1.2f, static_cast<float>(steps)), 1.2f, 30.0f);
            if (current_ != before) {
                Apply();
                if (sounds_ && wheelSounds_) {
                    sound::Play(sound::Effect::Tick);
                }
            }
        }
    }
    if (mode_ == 0 && Keybind() != keys::kNone && (GetAsyncKeyState(Keybind()) & 0x8000) == 0) {
        Manager::SetEnabled(*this, false, false);
    }
}

bool Zoom::DrawSettings() {
    bool changed = false;
    ui::Section("Style");
    int style = style_;
    if (ui::Choice("Style", &style, {"Normal", "Cinema", "Spyglass"})) {
        ApplyStyle(style);
        changed = true;
    }
    changed |= ui::Choice("Key", &mode_, {"Hold", "Toggle"});

    ui::Section("Zoom in");
    changed |= ui::Slider("Magnification", &factor_, 1.5f, 15.0f, "%.1fx");
    changed |= ui::Toggle("Wheel to zoom", &wheel_, "(while zooming)");
    if (wheel_) {
        changed |= ui::Toggle("Remember magnification", &keepWheel_, "(the wheel one)");
    }
    changed |= ui::Toggle("Smooth animation", &smooth_);
    if (smooth_) {
        changed |= ui::Slider("Speed", &smoothSpeed_, 2.0f, 30.0f, "%.0f");
    }

    ui::Section("Sensitivity");
    changed |= ui::Toggle("Follows zoom", &followSensitivity_, "(more zoom, slower camera)");
    if (followSensitivity_) {
        float percent = followStrength_ * 100.0f;
        if (ui::Slider("How much", &percent, 0.0f, 100.0f, "%.0f%%")) {
            followStrength_ = percent / 100.0f;
            changed = true;
        }
    }

    ui::Section("Effects");
    changed |= ui::Toggle("Cinema bars", &bars_);
    if (bars_) {
        float percent = barsHeight_ * 100.0f;
        if (ui::Slider("Bar height", &percent, 3.0f, 25.0f, "%.0f%%")) {
            barsHeight_ = percent / 100.0f;
            changed = true;
        }
    }
    changed |= ui::Toggle("Spyglass circle", &spyglass_);
    changed |= ui::Toggle("Hide Luma's HUD", &hideHud_);
    if (sdk::camera::HideHandAvailable()) {
        changed |= ui::Toggle("Hide hand", &hideHand_);
    }
    changed |= ui::Toggle("Hide the game UI", &hideGameUi_, "(presses F1 for you)");
    if (hideGameUi_) {
        ui::Hint("Luma presses F1 when the zoom starts and ends. If you press F1 in between, it flips.");
    }

    ui::Section("Sound");
    changed |= ui::Toggle("Sounds", &sounds_, "(zooming in and out)");
    if (sounds_) {
        changed |= ui::Toggle("Wheel click", &wheelSounds_);
    }

    if (changed) {
        if (!IsEnabled()) {
            current_ = factor_;
        } else {
            Apply();
            render::cinematic::SetBars(bars_ ? barsHeight_ : 0.0f);
            render::cinematic::SetSpyglass(spyglass_);
            render::cinematic::SetHideHud(hideHud_);
            sdk::camera::SetHideHand(hideHand_);
        }
        sdk::camera::SetZoomSmoothing(smooth_ ? smoothSpeed_ : 0.0f);
        sdk::player::SetZoomSensitivityFollow(followSensitivity_, followStrength_);
    }
    return changed;
}

void Zoom::SaveSettings(nlohmann::json& out) const {
    out["style"] = style_;
    out["mode"] = mode_;
    out["factor"] = factor_;
    out["wheel"] = wheel_;
    out["keepWheel"] = keepWheel_;
    out["smooth"] = smooth_;
    out["smoothSpeed"] = smoothSpeed_;
    out["followSensitivity"] = followSensitivity_;
    out["followStrength"] = followStrength_;
    out["bars"] = bars_;
    out["barsHeight"] = barsHeight_;
    out["spyglass"] = spyglass_;
    out["hideHud"] = hideHud_;
    out["hideHand"] = hideHand_;
    out["hideGameUi"] = hideGameUi_;
    out["sounds"] = sounds_;
    out["wheelSounds"] = wheelSounds_;
}

void Zoom::LoadSettings(const nlohmann::json& in) {
    style_ = std::clamp(in.value("style", 0), 0, 2);
    mode_ = std::clamp(in.value("mode", 0), 0, 1);
    factor_ = std::clamp(in.value("factor", 4.0f), 1.5f, 15.0f);
    current_ = factor_;
    wheel_ = in.value("wheel", true);
    keepWheel_ = in.value("keepWheel", false);
    smooth_ = in.value("smooth", true);
    smoothSpeed_ = std::clamp(in.value("smoothSpeed", 12.0f), 2.0f, 30.0f);
    followSensitivity_ = in.value("followSensitivity", in.value("lowerSensitivity", true));
    followStrength_ = std::clamp(in.value("followStrength", 1.0f), 0.0f, 1.0f);
    bars_ = in.value("bars", false);
    barsHeight_ = std::clamp(in.value("barsHeight", 0.11f), 0.03f, 0.25f);
    spyglass_ = in.value("spyglass", false);
    hideHud_ = in.value("hideHud", false);
    hideHand_ = in.value("hideHand", false);
    hideGameUi_ = in.value("hideGameUi", false);
    sounds_ = in.value("sounds", true);
    wheelSounds_ = in.value("wheelSounds", true);
    sdk::player::SetZoomSensitivityFollow(followSensitivity_, followStrength_);
}

// =============================================================================
//  FOV personalizado
// =============================================================================

FovChanger::FovChanger()
    : Module("fov_changer", "Custom FOV", "Pick your field of view, even outside the game's range.",
             Category::Visual) {}

const char* FovChanger::IconSvg() const {
    return render::icons::kAperture;
}

bool FovChanger::SdkReady() const {
    return sdk::camera::RenderAvailable();
}

void FovChanger::OnEnable() {
    sdk::camera::SetFovOverride(degrees_);
}

void FovChanger::OnDisable() {
    sdk::camera::SetFovOverride(0.0f);
}

bool FovChanger::DrawSettings() {
    const bool changed = ui::Slider("Field of view", &degrees_, 30.0f, 150.0f, "%.0f°");
    if (changed && IsEnabled()) {
        sdk::camera::SetFovOverride(degrees_);
    }
    ui::Hint("Vertical, in degrees. Replaces the game's (also the sprint effect).");
    return changed;
}

void FovChanger::SaveSettings(nlohmann::json& out) const {
    out["degrees"] = degrees_;
}

void FovChanger::LoadSettings(const nlohmann::json& in) {
    degrees_ = std::clamp(in.value("degrees", 90.0f), 30.0f, 150.0f);
    if (IsEnabled()) {
        sdk::camera::SetFovOverride(degrees_);
    }
}

}  // namespace luma::modules::visual
