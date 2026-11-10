// =============================================================================
//  camera_mods.cpp - Implementación de Fullbright y Freelook
// =============================================================================
#include "modules/visual/camera_mods.h"

#include <algorithm>
#include <cmath>

#include <windows.h>

#include "render/input.h"
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
    : Module("fullbright", "Fullbright", "Ves todo iluminado aunque sea de noche o estés en una cueva.",
             Category::Visual) {}

const char* Fullbright::IconSvg() const {
    return "M12 7C14.76 7 17 9.24 17 12C17 14.76 14.76 17 12 17C9.24 17 7 14.76 7 12C7 9.24 9.24 7 12 7Z "
           "M12 2v2 M12 20v2 M2 12h2 M20 12h2 M4.9 4.9l1.4 1.4 M17.7 17.7l1.4 1.4 M4.9 19.1l1.4-1.4 M17.7 6.3l1.4-1.4";
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
    const bool changed = ui::Slider("Brillo", &gamma_, 1.0f, 25.0f, "%.0f");
    if (changed && IsEnabled()) {
        sdk::camera::SetGammaOverride(gamma_);
    }
    ui::Hint("El brillo máximo del juego es 1. Con 10 o más, las cuevas se ven como de día.");
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
    : Module("freelook", "Freelook", "Mira alrededor en tercera persona sin girar al jugador. Asígnale una tecla.",
             Category::Visual) {}

const char* Freelook::IconSvg() const {
    return "M2 12C4.5 7 8 5 12 5C16 5 19.5 7 22 12C19.5 17 16 19 12 19C8 19 4.5 17 2 12Z M12 9v6 M9 12h6";
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
    changed |= ui::Choice("Tecla", &mode_, {"Mantener", "Alternar"});
    int index = view_ - 1;
    if (ui::Choice("Cámara", &index, {"Detrás", "Delante"})) {
        view_ = index + 1;
        changed = true;
        if (IsEnabled()) {
            sdk::camera::SetPerspectiveOverride(view_);
        }
    }
    ui::Hint(mode_ == 0 ? "Mantén apretada su tecla para mirar alrededor (asígnale una en General)."
                        : "Pulsa su tecla para activarlo y otra vez para desactivarlo.");
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

Zoom::Zoom()
    : Module("zoom", "Zoom", "Acerca la vista como un catalejo. Mantén su tecla (C) y usa la rueda para acercar más.",
             Category::Visual) {
    SetDefaultKeybind('C');
}

const char* Zoom::IconSvg() const {
    return "M16 10C16 13.31 13.31 16 10 16C6.69 16 4 13.31 4 10C4 6.69 6.69 4 10 4C13.31 4 16 6.69 16 10Z "
           "M14.5 14.5L20 20 M7.5 10h5 M10 7.5v5";
}

bool Zoom::SdkReady() const {
    return sdk::camera::CameraAvailable();
}

void Zoom::Apply() const {
    sdk::camera::SetZoomSmoothing(smooth_ ? smoothSpeed_ : 0.0f);
    sdk::camera::SetFovMultiplier(IsEnabled() ? 1.0f / current_ : 1.0f);
    // Con más aumento, la cámara se mueve más lento (si no, apuntar es imposible).
    sdk::player::SetZoomSensitivity(IsEnabled() && lowerSensitivity_ ? 1.0f / std::sqrt(current_) : 1.0f);
}

void Zoom::OnEnable() {
    if (!keepWheel_) {
        current_ = factor_;
    }
    render::input::SetWheelCapture(wheel_);
    render::input::TakeWheelSteps();  // descartar giros viejos
    Apply();
}

void Zoom::OnDisable() {
    render::input::SetWheelCapture(false);
    sdk::camera::SetFovMultiplier(1.0f);
    sdk::player::SetZoomSensitivity(1.0f);
}

void Zoom::OnTick() {
    if (wheel_) {
        const int steps = render::input::TakeWheelSteps();
        if (steps != 0) {
            current_ = std::clamp(current_ * std::pow(1.2f, static_cast<float>(steps)), 1.2f, 30.0f);
            Apply();
        }
    }
    if (mode_ == 0 && Keybind() != keys::kNone && (GetAsyncKeyState(Keybind()) & 0x8000) == 0) {
        Manager::SetEnabled(*this, false, false);
    }
}

bool Zoom::DrawSettings() {
    bool changed = false;
    changed |= ui::Choice("Tecla", &mode_, {"Mantener", "Alternar"});
    changed |= ui::Slider("Aumento", &factor_, 1.5f, 15.0f, "%.1fx");
    changed |= ui::Toggle("Rueda para acercar", &wheel_, "(mientras haces zoom)");
    if (wheel_) {
        changed |= ui::Toggle("Recordar el aumento", &keepWheel_, "(el de la rueda)");
    }
    changed |= ui::Toggle("Animación suave", &smooth_);
    if (smooth_) {
        changed |= ui::Slider("Velocidad", &smoothSpeed_, 3.0f, 30.0f, "%.0f");
    }
    changed |= ui::Toggle("Cámara más lenta con zoom", &lowerSensitivity_);
    if (changed) {
        if (!IsEnabled()) {
            current_ = factor_;
        }
        sdk::camera::SetZoomSmoothing(smooth_ ? smoothSpeed_ : 0.0f);
    }
    return changed;
}

void Zoom::SaveSettings(nlohmann::json& out) const {
    out["mode"] = mode_;
    out["factor"] = factor_;
    out["wheel"] = wheel_;
    out["keepWheel"] = keepWheel_;
    out["smooth"] = smooth_;
    out["smoothSpeed"] = smoothSpeed_;
    out["lowerSensitivity"] = lowerSensitivity_;
}

void Zoom::LoadSettings(const nlohmann::json& in) {
    mode_ = std::clamp(in.value("mode", 0), 0, 1);
    factor_ = std::clamp(in.value("factor", 4.0f), 1.5f, 15.0f);
    current_ = factor_;
    wheel_ = in.value("wheel", true);
    keepWheel_ = in.value("keepWheel", false);
    smooth_ = in.value("smooth", true);
    smoothSpeed_ = std::clamp(in.value("smoothSpeed", 12.0f), 3.0f, 30.0f);
    lowerSensitivity_ = in.value("lowerSensitivity", true);
}

// =============================================================================
//  FOV personalizado
// =============================================================================

FovChanger::FovChanger()
    : Module("fov_changer", "FOV personalizado", "Elige tu campo de visión, también fuera del rango del juego.",
             Category::Visual) {}

const char* FovChanger::IconSvg() const {
    return "M12 18L4 6 M12 18L20 6 M6.5 9.5C8 8.5 10 8 12 8C14 8 16 8.5 17.5 9.5";
}

bool FovChanger::SdkReady() const {
    return sdk::camera::CameraAvailable();
}

void FovChanger::OnEnable() {
    sdk::camera::SetFovOverride(degrees_);
}

void FovChanger::OnDisable() {
    sdk::camera::SetFovOverride(0.0f);
}

bool FovChanger::DrawSettings() {
    const bool changed = ui::Slider("Campo de visión", &degrees_, 30.0f, 150.0f, "%.0f°");
    if (changed && IsEnabled()) {
        sdk::camera::SetFovOverride(degrees_);
    }
    ui::Hint("Vertical, en grados. Reemplaza el del juego (también el efecto de correr).");
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
