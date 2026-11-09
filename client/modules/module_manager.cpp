// =============================================================================
//  module_manager.cpp - Implementación del gestor de módulos
// =============================================================================
#include "modules/module_manager.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <imgui.h>

#include "core/logger.h"
#include "render/notifications.h"
#include "render/sound.h"

namespace luma::modules {

// --- Module ------------------------------------------------------------------

Module::Module(std::string id, std::string name, std::string description, Category category)
    : id_(std::move(id)),
      name_(std::move(name)),
      description_(std::move(description)),
      category_(category) {}

const char* CategoryName(Category category) {
    switch (category) {
        case Category::Hud:         return "HUD";
        case Category::Visual:      return "Visual";
        case Category::Utility:     return "Utilidad";
        case Category::World:       return "Mundo";
        case Category::Performance: return "Rendimiento";
        case Category::Count:       break;
    }
    return "?";
}

// --- Manager -------------------------------------------------------------------

std::vector<std::unique_ptr<Module>> Manager::modules_;
std::vector<Module*> Manager::activeHud_;
std::atomic<bool> Manager::hasHud_{false};

const std::vector<std::unique_ptr<Module>>& Manager::All() {
    return modules_;
}

Module* Manager::Find(std::string_view id) {
    for (const auto& module : modules_) {
        if (module->Id() == id) {
            return module.get();
        }
    }
    return nullptr;
}

void Manager::RebuildActiveLists() {
    // activeHud_ tiene capacidad reservada (ver RegisterAll), así que clear y
    // push_back no piden memoria nueva.
    activeHud_.clear();
    for (const auto& module : modules_) {
        if (module->IsEnabled() && module->HasHud()) {
            activeHud_.push_back(module.get());
        }
    }
    hasHud_.store(!activeHud_.empty(), std::memory_order_relaxed);
}

void Manager::SetEnabled(Module& module, bool enabled, bool notify) {
    if (module.IsEnabled() == enabled) {
        return;
    }
    module.SetEnabledInternal(enabled);
    if (enabled) {
        module.OnEnable();
    } else {
        module.OnDisable();
    }
    RebuildActiveLists();
    log::Info("Módulo {}: {}", module.Name(), enabled ? "activado" : "desactivado");
    if (notify) {
        sound::Play(enabled ? sound::Effect::ToggleOn : sound::Effect::ToggleOff);
        notify::Push(enabled ? notify::Kind::Success : notify::Kind::Info,
                     std::format("{} {}", module.Name(), enabled ? "activado" : "desactivado"));
    }
}

bool Manager::HandleKey(int virtualKey) {
    if (virtualKey == keys::kNone) {
        return false;
    }
    bool changed = false;
    for (const auto& module : modules_) {
        if (module->Keybind() == virtualKey) {
            Toggle(*module, /*notify=*/true);
            changed = true;
        }
    }
    return changed;
}

void Manager::RenderHud(bool editing) {
    // La capa "de fondo" de ImGui: queda DEBAJO del menú y de los avisos.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    for (Module* module : activeHud_) {
        HudFrame frame;
        frame.draw = draw;
        frame.scale = module->layout_.scale;
        frame.editing = editing;
        if (module->IsHudElement()) {
            // De la posición guardada (fracciones + ancla) a píxeles. Usamos
            // el tamaño del frame anterior: el elemento todavía no se dibujó.
            const HudLayout& l = module->layout_;
            const ImVec2 size = module->lastSize_;
            float x = l.x * display.x - l.anchorX * size.x;
            float y = l.y * display.y - l.anchorY * size.y;
            // Nunca fuera de la pantalla (por ejemplo, tras bajar la resolución).
            x = std::clamp(x, 0.0f, std::max(0.0f, display.x - size.x));
            y = std::clamp(y, 0.0f, std::max(0.0f, display.y - size.y));
            frame.pos = ImVec2(std::floor(x), std::floor(y));
        }
        const ImVec2 size = module->OnRenderHud(frame);
        module->lastPos_ = frame.pos;
        module->lastSize_ = size;
    }
}

void Manager::ResetLayout(Module& module) {
    module.layout_ = module.defaultLayout_;
}

void Manager::Save(nlohmann::json& out) {
    nlohmann::json all = nlohmann::json::object();
    for (const auto& module : modules_) {
        nlohmann::json entry;
        entry["enabled"] = module->IsEnabled();
        entry["keybind"] = module->Keybind();
        nlohmann::json settings = nlohmann::json::object();
        module->SaveSettings(settings);
        entry["settings"] = std::move(settings);
        if (module->IsHudElement()) {
            const HudLayout& l = module->layout_;
            entry["hud"] = {{"x", l.x}, {"y", l.y}, {"anchorX", l.anchorX}, {"anchorY", l.anchorY}, {"scale", l.scale}};
        }
        all[module->Id()] = std::move(entry);
    }
    out["modules"] = std::move(all);
}

void Manager::Load(const nlohmann::json& in) {
    const nlohmann::json* all = nullptr;
    if (in.is_object() && in.contains("modules") && in["modules"].is_object()) {
        all = &in["modules"];
    }
    for (const auto& module : modules_) {
        bool enabled = false;
        int keybind = keys::kNone;
        module->layout_ = module->defaultLayout_;
        if (all != nullptr && all->contains(module->Id())) {
            const nlohmann::json& entry = (*all)[module->Id()];
            // .value(clave, por_defecto) devuelve el valor guardado, o el
            // valor por defecto si no está. Si el tipo es incorrecto lanza una
            // excepción, que atrapa config.cpp (y usa la config por defecto).
            enabled = entry.value("enabled", false);
            keybind = entry.value("keybind", keys::kNone);
            if (entry.contains("settings") && entry["settings"].is_object()) {
                module->LoadSettings(entry["settings"]);
            }
            if (entry.contains("hud") && entry["hud"].is_object()) {
                const nlohmann::json& h = entry["hud"];
                HudLayout& l = module->layout_;
                const HudLayout& d = module->defaultLayout_;
                l.x = std::clamp(h.value("x", d.x), 0.0f, 1.0f);
                l.y = std::clamp(h.value("y", d.y), 0.0f, 1.0f);
                l.anchorX = std::clamp(h.value("anchorX", d.anchorX), 0.0f, 1.0f);
                l.anchorY = std::clamp(h.value("anchorY", d.anchorY), 0.0f, 1.0f);
                l.scale = std::clamp(h.value("scale", d.scale), 0.5f, 4.0f);
            }
        }
        module->SetKeybind(keybind);
        SetEnabled(*module, enabled, /*notify=*/false);
    }
}

void Manager::Shutdown() {
    for (const auto& module : modules_) {
        if (module->IsEnabled()) {
            module->SetEnabledInternal(false);
            module->OnDisable();
        }
    }
    activeHud_.clear();
    hasHud_.store(false);
    modules_.clear();
}

}  // namespace luma::modules
