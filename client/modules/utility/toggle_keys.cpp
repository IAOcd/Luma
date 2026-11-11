// =============================================================================
//  toggle_keys.cpp - Implementación de toggle sprint / toggle sneak
// =============================================================================
#include "modules/utility/toggle_keys.h"
#include "core/tr.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "modules/module_manager.h"
#include "render/hud.h"
#include "render/ui.h"

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
    {VK_CONTROL, "Ctrl"}, {VK_SHIFT, "Shift"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "Caps Lock"},
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
    // Las cabezas son círculos hechos con curvas C: el dibujador de íconos de
    // Luma no entiende los arcos (comando a de SVG), por eso antes no se veían.
    return IsSprint(slot_)
               // corriendo: inclinado hacia adelante, con una pierna atrás
               ? "M13.5 3C14.6 3 15.5 3.9 15.5 5S14.6 7 13.5 7S11.5 6.1 11.5 5S12.4 3 13.5 3z "
                 "M12.5 8.5l-2.5 4.5 3 2-1 5.5 M10 13l-4 1 M12.5 8.5l3.5 3 3-1 M13 15l4 2"
               // agachado: más bajo, con las rodillas dobladas
               : "M12 6C13.1 6 14 6.9 14 8S13.1 10 12 10S10 9.1 10 8S10.9 6 12 6z "
                 "M12 11l-1 4.5 4 1.5 1 3.5 M11 15.5l-3 4.5 M12 11.5l-4 2 M12 11.5l3.5 1.5";
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
    const char* text = label_[0] != 0 ? label_ : IsSprint(slot_) ? Tr("[Sprinting (toggled)]") : Tr("[Sneaking (toggled)]");
    const rh::Metrics m = rh::Measure(frame.scale);
    const ImVec2 size = rh::TextSize(m, text);
    // Sin caja: solo texto con sombra (como el aviso de otros clients).
    rh::Text(frame.draw, m, ImVec2(frame.pos.x, frame.pos.y - m.capTop), text, rh::TextColor(active ? 1.0f : 0.5f));
    return ImVec2(size.x, m.capHeight + m.px);
}

bool ToggleKey::DrawSettings() {
    namespace ui = render::ui;
    bool changed = false;
    if (ui::Choice(IsSprint(slot_) ? "Sprint key" : "Sneak key", &keyIndex_,
                   {"Ctrl", "Shift", "Alt", "Caps Lock", "C", "R", "X", "Z"})) {
        changed = true;
        if (IsEnabled()) {
            render::input::ConfigureHold(slot_, true, GameKey());
        }
    }
    ui::Hint("It has to be the same key you use in the game (Settings > Controls).");
    if (ui::Toggle("Text on the HUD", &showText_)) {
        changed = true;
        Manager::RefreshActive();  // agrega o quita el módulo de la lista de dibujo
    }
    if (showText_) {
        changed |= ui::Text("Text", label_, sizeof(label_), IsSprint(slot_) ? "[Sprinting (toggled)]" : "[Sneaking (toggled)]");
    }
    return changed;
}

void ToggleKey::SaveSettings(nlohmann::json& out) const {
    out["key"] = GameKey();
    out["showText"] = showText_;
    out["label"] = label_;
}

void ToggleKey::LoadSettings(const nlohmann::json& in) {
    const int vk = in.value("key", IsSprint(slot_) ? VK_CONTROL : VK_SHIFT);
    for (int i = 0; i < kKeyCount; ++i) {
        if (kKeys[i].vk == vk) {
            keyIndex_ = i;
        }
    }
    showText_ = in.value("showText", true);
    std::snprintf(label_, sizeof(label_), "%s", in.value("label", std::string()).c_str());
    if (IsEnabled()) {
        render::input::ConfigureHold(slot_, true, GameKey());  // por si cambió la tecla
    }
}

}  // namespace luma::modules::utility
