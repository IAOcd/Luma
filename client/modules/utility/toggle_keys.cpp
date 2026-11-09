// =============================================================================
//  toggle_keys.cpp - Implementación de toggle sprint / toggle sneak
// =============================================================================
#include "modules/utility/toggle_keys.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>

#include "modules/module_manager.h"
#include "render/hud.h"

namespace luma::modules::utility {

namespace rh = render::hud;
using render::input::HoldSlot;

namespace {

// Las teclas que se pueden elegir: tienen que coincidir con las que usa el
// juego para correr / agacharse (en Opciones > Controles > Teclado).
struct KeyOption {
    int vk;
    const char* name;
};
constexpr KeyOption kKeys[] = {
    {VK_CONTROL, "Ctrl"}, {VK_SHIFT, "Shift"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "Bloq Mayús"},
    {'C', "C"},           {'R', "R"},          {'X', "X"},       {'Z', "Z"},
};
constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

bool IsSprint(HoldSlot slot) {
    return slot == HoldSlot::Sprint;
}

}  // namespace

ToggleKey::ToggleKey(HoldSlot slot)
    : Module(IsSprint(slot) ? "toggle_sprint" : "toggle_sneak", IsSprint(slot) ? "Toggle sprint" : "Toggle sneak",
             IsSprint(slot) ? "Corre sin mantener Ctrl: Luma lo mantiene por ti. Pulsa la tecla de correr para "
                              "alternar."
                            : "Agáchate sin mantener Shift: Luma lo mantiene por ti. Pulsa la tecla para alternar. "
                              "Se suelta solo en el inventario y el chat.",
             Category::Utility),
      slot_(slot),
      keyIndex_(IsSprint(slot) ? 0 : 1) {
    // Texto arriba a la derecha, debajo del overlay de rendimiento.
    HudLayout l;
    l.x = 0.996f;
    l.y = IsSprint(slot) ? 0.20f : 0.235f;
    l.anchorX = 1.0f;
    SetDefaultLayout(l);
}

const char* ToggleKey::IconSvg() const {
    return IsSprint(slot_) ? "M13 4.5a1.5 1.5 0 1 0 0.01 0 M9 20l3-6 3 2v5 M6 12l3-3h5l3 3 M11 9l-2 5"
                           : "M12 4.5a1.5 1.5 0 1 0 0.01 0 M8 20l2-5 4 1 2 4 M8 12h8 M10 8l-2 4";
}

int ToggleKey::GameKey() const {
    return kKeys[std::clamp(keyIndex_, 0, kKeyCount - 1)].vk;
}

void ToggleKey::OnEnable() {
    render::input::ConfigureHold(slot_, true, GameKey());
}

void ToggleKey::OnDisable() {
    render::input::ConfigureHold(slot_, false, GameKey());
}

ImVec2 ToggleKey::OnRenderHud(const HudFrame& frame) {
    const bool active = render::input::IsHoldActive(slot_);
    if (!active && !frame.editing) {
        return ImVec2(0, 0);
    }
    const char* text = IsSprint(slot_) ? "[Corriendo (alternado)]" : "[Agachado (alternado)]";
    const rh::Metrics m = rh::Measure(frame.scale);
    const ImVec2 size = rh::TextSize(m, text);
    // Sin caja: solo texto con sombra (como el aviso de otros clients).
    rh::Text(frame.draw, m, ImVec2(frame.pos.x, frame.pos.y - m.capTop), text, rh::TextColor(active ? 1.0f : 0.5f));
    return ImVec2(size.x, m.capHeight + m.px);
}

bool ToggleKey::DrawSettings() {
    bool changed = false;
    const char* preview = kKeys[std::clamp(keyIndex_, 0, kKeyCount - 1)].name;
    if (ImGui::BeginCombo(IsSprint(slot_) ? "Tecla de correr del juego" : "Tecla de agacharse del juego", preview)) {
        for (int i = 0; i < kKeyCount; ++i) {
            if (ImGui::Selectable(kKeys[i].name, i == keyIndex_) && i != keyIndex_) {
                keyIndex_ = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (changed && IsEnabled()) {
        render::input::ConfigureHold(slot_, true, GameKey());
    }
    if (ImGui::Checkbox("Mostrar texto en el HUD", &showText_)) {
        changed = true;
        Manager::RefreshActive();  // agrega o quita el módulo de la lista de dibujo
    }
    ImGui::TextDisabled("Tiene que ser la misma tecla que usas en el juego.");
    return changed;
}

void ToggleKey::SaveSettings(nlohmann::json& out) const {
    out["key"] = GameKey();
    out["showText"] = showText_;
}

void ToggleKey::LoadSettings(const nlohmann::json& in) {
    const int vk = in.value("key", IsSprint(slot_) ? VK_CONTROL : VK_SHIFT);
    for (int i = 0; i < kKeyCount; ++i) {
        if (kKeys[i].vk == vk) {
            keyIndex_ = i;
        }
    }
    showText_ = in.value("showText", true);
    if (IsEnabled()) {
        render::input::ConfigureHold(slot_, true, GameKey());  // por si cambió la tecla
    }
}

}  // namespace luma::modules::utility
