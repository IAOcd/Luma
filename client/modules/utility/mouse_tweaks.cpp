// =============================================================================
//  mouse_tweaks.cpp - Implementación de los ajustes del mouse
// =============================================================================
#include "modules/utility/mouse_tweaks.h"

#include "render/icons.h"

#include <imgui.h>

#include <algorithm>

#include "render/ui.h"
#include "sdk/mouse_queue.h"
#include "sdk/player.h"

namespace luma::modules::utility {

// --- Desactivar la rueda ----------------------------------------------------------

ScrollWheelDisabler::ScrollWheelDisabler()
    : Module("scroll_disabler", "Disable scroll wheel",
             "While playing, the mouse wheel doesn't change the hotbar slot.", Category::Utility) {}

const char* ScrollWheelDisabler::IconSvg() const {
    return render::icons::kMouseOff;
}

void ScrollWheelDisabler::OnEnable() {
    sdk::mousequeue::SetBlockWheel(true);
}

void ScrollWheelDisabler::OnDisable() {
    sdk::mousequeue::SetBlockWheel(false);
}

// --- Multiplicador de sensibilidad ---------------------------------------------------

SensitivityMultiplier::SensitivityMultiplier()
    : Module("sensitivity", "Sensitivity multiplier",
             "Multiplies the game's camera sensitivity (for example, 0.5 = half).", Category::Utility) {}

const char* SensitivityMultiplier::IconSvg() const {
    return render::icons::kSliders;
}

void SensitivityMultiplier::OnEnable() {
    sdk::player::SetSensitivity(multiplier_);
}

void SensitivityMultiplier::OnDisable() {
    sdk::player::SetSensitivity(1.0f);
}

bool SensitivityMultiplier::DrawSettings() {
    const bool changed = render::ui::Slider("Multiplier", &multiplier_, 0.1f, 3.0f, "%.2fx");
    if (changed && IsEnabled()) {
        sdk::player::SetSensitivity(multiplier_);
    }
    render::ui::Hint("1.00x = same as the game; 0.50x = half as fast.");
    return changed;
}

void SensitivityMultiplier::SaveSettings(nlohmann::json& out) const {
    out["multiplier"] = multiplier_;
}

void SensitivityMultiplier::LoadSettings(const nlohmann::json& in) {
    multiplier_ = std::clamp(in.value("multiplier", 0.5f), 0.1f, 3.0f);
    if (IsEnabled()) {
        sdk::player::SetSensitivity(multiplier_);
    }
}

// --- Límite de CPS -------------------------------------------------------------------

CpsCap::CpsCap()
    : Module("cps_cap", "CPS limit",
             "Drops clicks above a maximum per second (some servers require it).", Category::Utility) {}

const char* CpsCap::IconSvg() const {
    return render::icons::kMouseBan;
}

void CpsCap::OnEnable() {
    sdk::mousequeue::SetCpsCap(cap_);
}

void CpsCap::OnDisable() {
    sdk::mousequeue::SetCpsCap(0);
}

bool CpsCap::DrawSettings() {
    const bool changed = render::ui::SliderInt("Maximum", &cap_, 5, 30, "%d CPS");
    if (changed && IsEnabled()) {
        sdk::mousequeue::SetCpsCap(cap_);
    }
    return changed;
}

void CpsCap::SaveSettings(nlohmann::json& out) const {
    out["cap"] = cap_;
}

void CpsCap::LoadSettings(const nlohmann::json& in) {
    cap_ = std::clamp(in.value("cap", 12), 5, 30);
    if (IsEnabled()) {
        sdk::mousequeue::SetCpsCap(cap_);
    }
}

}  // namespace luma::modules::utility
