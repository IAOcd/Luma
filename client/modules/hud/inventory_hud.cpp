// =============================================================================
//  inventory_hud.cpp - Ítem en la mano, contador de ítems y aviso de durabilidad
// =============================================================================
#include "modules/hud/inventory_hud.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

#include "perf/timer.h"
#include "render/hud.h"
#include "render/item_icons.h"
#include "render/notifications.h"
#include "render/sound.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace inv = sdk::inventory;

namespace {

// ¿Pasaron "ms" milisegundos desde "last"? (y actualiza "last")
bool Every(std::int64_t& last, double ms) {
    const std::int64_t now = perf::Now();
    if (last != 0 && perf::TicksToMs(now - last) < ms) {
        return false;
    }
    last = now;
    return true;
}

// Color de la durabilidad: verde -> amarillo -> rojo.
ImU32 DurabilityColor(float fraction, float alpha = 1.0f) {
    const float r = fraction > 0.5f ? (1.0f - fraction) * 2.0f : 1.0f;
    const float g = fraction > 0.5f ? 1.0f : fraction * 2.0f;
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, 0.25f, alpha));
}


}  // namespace

// =============================================================================
//  Ítem en la mano
// =============================================================================

HeldItem::HeldItem()
    : Module("held_item", "Ítem en la mano", "Nombre, cantidad y durabilidad del ítem que tienes seleccionado.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.5f;
    l.y = 0.86f;
    l.anchorX = 0.5f;
    l.anchorY = 1.0f;
    SetDefaultLayout(l);
}

const char* HeldItem::IconSvg() const {
    return "M14.5 4.5l5 5 M5 19l9.5-9.5 M12 7l5 5 M4 20l2-1 M5 15l4 4";
}

ImVec2 HeldItem::OnRenderHud(const HudFrame& frame) {
    if (Every(lastRead_, 100.0)) {
        const int slot = inv::SelectedSlot();
        hasItem_ = slot >= 0 && inv::ReadSlot(slot, item_);
    }
    inv::Item item = item_;
    if (!hasItem_) {
        if (!frame.editing) {
            return ImVec2(0, 0);
        }
        std::snprintf(item.id, sizeof(item.id), "minecraft:diamond_sword");  // ejemplo para el editor
        item.count = 1;
        item.maxDamage = 1561;
        item.damage = 200;
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    char name[64];
    inv::PrettyName(item.id, name, sizeof(name));
    char text[96];
    if (item.maxDamage > 0) {
        const int left = item.maxDamage - item.damage;
        if (percent_) {
            std::snprintf(text, sizeof(text), "%s  %d%%", name, left * 100 / item.maxDamage);
        } else {
            std::snprintf(text, sizeof(text), "%s  %d/%d", name, left, item.maxDamage);
        }
    } else if (item.count > 1) {
        std::snprintf(text, sizeof(text), "%s  x%d", name, item.count);
    } else {
        std::snprintf(text, sizeof(text), "%s", name);
    }
    const bool bar = showBar_ && item.maxDamage > 0;
    const ImVec2 size = rh::TextBox(frame.draw, m, frame.pos, text, 58.0f);
    if (!bar) {
        return size;
    }
    // Barra de durabilidad debajo, del ancho de la caja.
    const float fraction = static_cast<float>(item.maxDamage - item.damage) / static_cast<float>(item.maxDamage);
    const ImVec2 b0(frame.pos.x, frame.pos.y + size.y);
    const ImVec2 b1(frame.pos.x + size.x, b0.y + 2.0f * u);
    frame.draw->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 160));
    frame.draw->AddRectFilled(b0, ImVec2(b0.x + std::floor(size.x * fraction), b1.y), DurabilityColor(fraction));
    return ImVec2(size.x, size.y + 2.0f * u);
}

bool HeldItem::DrawSettings() {
    bool changed = false;
    changed |= ImGui::Checkbox("Barra de durabilidad", &showBar_);
    changed |= ImGui::Checkbox("Durabilidad en porcentaje", &percent_);
    return changed;
}

void HeldItem::SaveSettings(nlohmann::json& out) const {
    out["showBar"] = showBar_;
    out["percent"] = percent_;
}

void HeldItem::LoadSettings(const nlohmann::json& in) {
    showBar_ = in.value("showBar", true);
    percent_ = in.value("percent", false);
}

// =============================================================================
//  Contador de ítems
// =============================================================================

ItemCounter::ItemCounter()
    : Module("item_counter", "Contador de ítems",
             "Cuenta los ítems que elijas en todo el inventario, cada uno en su caja con su ícono.", Category::Hud) {
    HudLayout l;
    l.x = 0.004f;
    l.y = 0.25f;
    SetDefaultLayout(l);
    tracked_ = {"minecraft:arrow", "minecraft:ender_pearl", "minecraft:golden_apple", "minecraft:totem_of_undying"};
}

const char* ItemCounter::IconSvg() const {
    return "M4 8l8-4 8 4-8 4z M4 8v8l8 4 8-4V8 M12 12v8";
}

namespace {

// 1234567 -> "1.234.567"
void Thousands(int value, char* out, std::size_t size) {
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%d", value);
    const int len = static_cast<int>(std::strlen(digits));
    std::size_t n = 0;
    for (int i = 0; i < len && n + 2 < size; ++i) {
        if (i > 0 && (len - i) % 3 == 0) {
            out[n++] = '.';
        }
        out[n++] = digits[i];
    }
    out[n] = 0;
}

}  // namespace

ImVec2 ItemCounter::OnRenderHud(const HudFrame& frame) {
    const int tracked = std::min(static_cast<int>(tracked_.size()), kMaxTracked);
    if (Every(lastRead_, 250.0)) {
        // Una sola pasada por los 36 lugares (y la mano secundaria), sumando
        // para cada ítem elegido.
        std::fill(std::begin(counts_), std::end(counts_), 0);
        auto add = [&](const inv::Item& item) {
            for (int i = 0; i < tracked; ++i) {
                if (tracked_[static_cast<std::size_t>(i)] == item.id) {
                    counts_[i] += item.count;
                }
            }
        };
        inv::Item item;
        for (int slot = 0; slot < inv::kSlots; ++slot) {
            if (inv::ReadSlot(slot, item)) {
                add(item);
            }
        }
        if (inv::ReadOffhand(item)) {
            add(item);
        }
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float boxH = std::floor(rh::LineBoxHeight(m) + 2.0f * u);
    const float icon = std::floor(boxH - 6.0f * u);
    const float gap = 2.0f * u;
    float y = frame.pos.y;
    float width = 0.0f;
    for (int i = 0; i < tracked; ++i) {
        if (hideZero_ && counts_[i] == 0 && !frame.editing) {
            continue;
        }
        char text[24];
        Thousands(counts_[i], text, sizeof(text));
        const float textW = rh::TextSize(m, text).x;
        const float w = std::floor(4.0f * u + icon + 4.0f * u + textW + 6.0f * u);
        const ImVec2 min(frame.pos.x, y);
        const ImVec2 max(min.x + w, y + boxH);
        rh::Box(frame.draw, m, min, max);
        // El ícono del ítem, leído de tu instalación del juego (si no hay,
        // un cuadradito tenue en su lugar).
        const ImVec2 i0(min.x + 4.0f * u, std::floor(y + (boxH - icon) * 0.5f));
        const ImVec2 i1(i0.x + icon, i0.y + icon);
        if (ImTextureID texture = render::icons_items::Get(tracked_[static_cast<std::size_t>(i)].c_str())) {
            frame.draw->AddImage(texture, i0, i1);
        } else {
            frame.draw->AddRectFilled(i0, i1, IM_COL32(255, 255, 255, 30));
        }
        rh::Text(frame.draw, m, ImVec2(i1.x + 4.0f * u, y + (boxH - m.capHeight) * 0.5f - m.capTop), text,
                 rh::TextColor());
        width = std::max(width, w);
        y += boxH + gap;
    }
    if (y == frame.pos.y) {
        return ImVec2(0, 0);
    }
    return ImVec2(width, y - gap - frame.pos.y);
}

bool ItemCounter::DrawSettings() {
    bool changed = false;
    ImGui::TextUnformatted("Ítems que se cuentan:");
    int remove = -1;
    for (int i = 0; i < static_cast<int>(tracked_.size()); ++i) {
        ImGui::PushID(i);
        char name[64];
        inv::PrettyName(tracked_[static_cast<std::size_t>(i)].c_str(), name, sizeof(name));
        if (ImGui::SmallButton("Quitar")) {
            remove = i;
        }
        ImGui::SameLine();
        ImGui::Text("%s", name);
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", tracked_[static_cast<std::size_t>(i)].c_str());
        ImGui::PopID();
    }
    if (remove >= 0) {
        tracked_.erase(tracked_.begin() + remove);
        changed = true;
    }
    if (static_cast<int>(tracked_.size()) < kMaxTracked) {
        static const char* const kPresets[] = {
            "minecraft:arrow",        "minecraft:ender_pearl",      "minecraft:golden_apple",
            "minecraft:enchanted_golden_apple", "minecraft:totem_of_undying", "minecraft:diamond",
            "minecraft:emerald",      "minecraft:iron_ingot",       "minecraft:gold_ingot",
            "minecraft:netherite_ingot", "minecraft:coal",          "minecraft:copper_ingot",
            "minecraft:wind_charge",  "minecraft:firework_rocket",  "minecraft:experience_bottle",
            "minecraft:obsidian",     "minecraft:end_crystal",      "minecraft:cobblestone",
            "minecraft:tnt",          "minecraft:snowball"};
        if (ImGui::BeginCombo("Agregar", "Elegir un ítem...")) {
            for (const char* id : kPresets) {
                char name[64];
                inv::PrettyName(id, name, sizeof(name));
                if (ImGui::Selectable(name)) {
                    tracked_.emplace_back(id);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        // Cualquier otro: escribiendo su id (como en los comandos del juego).
        ImGui::InputTextWithHint("##custom", "o escribe un id: minecraft:...", custom_, sizeof(custom_));
        ImGui::SameLine();
        if (ImGui::Button("Agregar id") && custom_[0] != 0) {
            std::string id = custom_;
            if (id.find(':') == std::string::npos) {
                id = "minecraft:" + id;
            }
            tracked_.push_back(id);
            custom_[0] = 0;
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("Esconder los que tienes en 0", &hideZero_);
    if (changed) {
        lastRead_ = 0;
    }
    return changed;
}

void ItemCounter::SaveSettings(nlohmann::json& out) const {
    out["items"] = tracked_;
    out["hideZero"] = hideZero_;
}

void ItemCounter::LoadSettings(const nlohmann::json& in) {
    if (in.contains("items") && in["items"].is_array()) {
        tracked_.clear();
        for (const auto& v : in["items"]) {
            if (v.is_string() && static_cast<int>(tracked_.size()) < kMaxTracked) {
                tracked_.push_back(v.get<std::string>());
            }
        }
    }
    hideZero_ = in.value("hideZero", false);
}

// =============================================================================
//  Armadura
// =============================================================================

ArmorHud::ArmorHud()
    : Module("armor_hud", "Armadura", "Tu armadura puesta (y lo que tienes en las manos) con su durabilidad.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.004f;
    l.y = 0.55f;
    l.anchorY = 0.5f;
    SetDefaultLayout(l);
}

const char* ArmorHud::IconSvg() const {
    return "M12 3l7 3v5c0 4.5-3 8.5-7 10-4-1.5-7-5.5-7-10V6z";
}

ImVec2 ArmorHud::OnRenderHud(const HudFrame& frame) {
    if (Every(lastRead_, 250.0)) {
        for (int i = 0; i < 4; ++i) {
            has_[i] = inv::ReadArmor(i, items_[i]);
        }
        const int slot = inv::SelectedSlot();
        has_[4] = showHeld_ && slot >= 0 && inv::ReadSlot(slot, items_[4]);
        has_[5] = showHeld_ && inv::ReadOffhand(items_[5]);
    }
    // Ejemplo para el editor si no tienes nada puesto.
    static const char* const kSample[] = {"minecraft:diamond_helmet", "minecraft:diamond_chestplate",
                                          "minecraft:diamond_leggings", "minecraft:diamond_boots"};
    inv::Item shown[6];
    bool any = false;
    for (int i = 0; i < 6; ++i) {
        any |= has_[i];
    }
    if (!any && !frame.editing) {
        return ImVec2(0, 0);
    }
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float pad = 5.0f * u;
    const float line = m.capHeight + 4.0f * u;
    char names[6][64];
    char values[6][24];
    int count = 0;
    float nameWidth = 0.0f;
    float valueWidth = 0.0f;
    for (int i = 0; i < 6; ++i) {
        inv::Item item = items_[i];
        if (!any && i < 4) {
            std::snprintf(item.id, sizeof(item.id), "%s", kSample[i]);
            item.count = 1;
            item.maxDamage = 363;
            item.damage = 40;
        } else if (!has_[i]) {
            continue;
        }
        shown[count] = item;
        inv::PrettyName(item.id, names[count], sizeof(names[count]));
        if (item.maxDamage > 0) {
            const int left = item.maxDamage - item.damage;
            if (percent_) {
                std::snprintf(values[count], sizeof(values[count]), "%d%%", left * 100 / item.maxDamage);
            } else {
                std::snprintf(values[count], sizeof(values[count]), "%d", left);
            }
        } else if (item.count > 1) {
            std::snprintf(values[count], sizeof(values[count]), "x%d", item.count);
        } else {
            values[count][0] = 0;
        }
        nameWidth = std::max(nameWidth, rh::TextSize(m, names[count]).x);
        valueWidth = std::max(valueWidth, rh::TextSize(m, values[count]).x);
        ++count;
    }
    const float width = std::floor(pad * 2.0f + nameWidth + 6.0f * u + valueWidth);
    const float height = std::floor(pad * 2.0f + line * static_cast<float>(count) - 4.0f * u);
    const ImVec2 o = frame.pos;
    rh::Box(frame.draw, m, o, ImVec2(o.x + width, o.y + height));
    for (int i = 0; i < count; ++i) {
        const float y = o.y + pad + line * static_cast<float>(i) - m.capTop;
        rh::Text(frame.draw, m, ImVec2(o.x + pad, y), names[i], rh::TextColor());
        // El número a la derecha, del color de su durabilidad.
        const inv::Item& item = shown[i];
        const ImU32 color = item.maxDamage > 0
                                ? DurabilityColor(static_cast<float>(item.maxDamage - item.damage) / static_cast<float>(item.maxDamage))
                                : rh::TextColor(0.8f);
        const float vw = rh::TextSize(m, values[i]).x;
        rh::Text(frame.draw, m, ImVec2(o.x + width - pad - vw, y), values[i], color);
    }
    return ImVec2(width, height);
}

bool ArmorHud::DrawSettings() {
    bool changed = false;
    changed |= ImGui::Checkbox("Incluir lo que tienes en las manos", &showHeld_);
    changed |= ImGui::Checkbox("Durabilidad en porcentaje", &percent_);
    if (changed) {
        lastRead_ = 0;
    }
    return changed;
}

void ArmorHud::SaveSettings(nlohmann::json& out) const {
    out["showHeld"] = showHeld_;
    out["percent"] = percent_;
}

void ArmorHud::LoadSettings(const nlohmann::json& in) {
    showHeld_ = in.value("showHeld", true);
    percent_ = in.value("percent", false);
}

// =============================================================================
//  Aviso de durabilidad baja
// =============================================================================

DurabilityWarning::DurabilityWarning()
    : Module("durability_warning", "Aviso de durabilidad baja",
             "Te avisa con un mensaje y un sonido cuando el ítem en la mano está por romperse.", Category::Utility) {}

const char* DurabilityWarning::IconSvg() const {
    return "M12 3L2 20h20L12 3Z M12 9v5 M12 17v0.5";
}

void DurabilityWarning::OnTick() {
    const int slot = inv::SelectedSlot();
    inv::Item item;
    if (slot < 0 || !inv::ReadSlot(slot, item) || item.maxDamage <= 0) {
        return;
    }
    const int left = item.maxDamage - item.damage;
    const int percent = left * 100 / item.maxDamage;
    const bool low = percent <= threshold_;
    const bool sameItem = slot == warnedSlot_ && std::strcmp(item.id, warnedId_) == 0;
    if (low && !sameItem) {
        // Avisar una vez por ítem (no cada 50 ms).
        char name[64];
        inv::PrettyName(item.id, name, sizeof(name));
        notify::Push(notify::Kind::Warning, std::format("{}: durabilidad baja ({} usos, {}%)", name, left, percent));
        sound::Play(sound::Effect::ToggleOff);
        warnedSlot_ = slot;
        std::snprintf(warnedId_, sizeof(warnedId_), "%s", item.id);
    } else if (!low && sameItem) {
        warnedSlot_ = -1;  // lo reparaste: puede volver a avisar
        warnedId_[0] = 0;
    }
}

bool DurabilityWarning::DrawSettings() {
    return ImGui::SliderInt("Avisar debajo de", &threshold_, 1, 50, "%d%%");
}

void DurabilityWarning::SaveSettings(nlohmann::json& out) const {
    out["threshold"] = threshold_;
}

void DurabilityWarning::LoadSettings(const nlohmann::json& in) {
    threshold_ = std::clamp(in.value("threshold", 10), 1, 50);
}

}  // namespace luma::modules::hud
