// =============================================================================
//  inventory_hud.cpp - Armadura, ítem en la mano, contador y aviso de durabilidad
// =============================================================================
#include "modules/hud/inventory_hud.h"

#include "render/icons.h"
#include "core/tr.h"

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
#include "render/theme.h"
#include "render/ui.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace inv = sdk::inventory;
namespace ui = render::ui;

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

float Fraction(const inv::Item& item) {
    return item.maxDamage > 0 ? static_cast<float>(item.maxDamage - item.damage) / static_cast<float>(item.maxDamage)
                              : 1.0f;
}

// Dibuja el ícono de un ítem (o un cuadradito tenue si no hay ícono).
void DrawItemIcon(ImDrawList* draw, const char* id, ImVec2 min, float size, const char* atlas = nullptr,
                  int color = -1) {
    const ImVec2 max(min.x + size, min.y + size);
    if (ImTextureID texture = render::icons_items::Get(id, atlas, color)) {
        draw->AddImage(texture, min, max);
    } else {
        draw->AddRectFilled(min, max, IM_COL32(255, 255, 255, 30));
    }
}

// 1234567 -> "1.234.567" / "1.2M" / "1234567"
void FormatCount(int value, int format, char* out, std::size_t size) {
    if (format == 1 && value >= 1000) {
        const double v = value >= 1000000 ? value / 1000000.0 : value / 1000.0;
        std::snprintf(out, size, v < 10.0 ? "%.1f%s" : "%.0f%s", v, value >= 1000000 ? "M" : "K");
        return;
    }
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%d", value);
    if (format == 2) {
        std::snprintf(out, size, "%s", digits);
        return;
    }
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

// Ítems que se ofrecen en el selector (con ícono). Cualquier otro se puede
// agregar escribiendo su id.
constexpr const char* kPickerItems[] = {
    "minecraft:arrow",          "minecraft:spectral_arrow",   "minecraft:ender_pearl",
    "minecraft:golden_apple",   "minecraft:enchanted_golden_apple", "minecraft:totem_of_undying",
    "minecraft:diamond",        "minecraft:emerald",          "minecraft:iron_ingot",
    "minecraft:gold_ingot",     "minecraft:netherite_ingot",  "minecraft:copper_ingot",
    "minecraft:coal",           "minecraft:redstone",         "minecraft:lapis_lazuli",
    "minecraft:experience_bottle", "minecraft:firework_rocket", "minecraft:wind_charge",
    "minecraft:snowball",       "minecraft:egg",              "minecraft:bread",
    "minecraft:cooked_beef",    "minecraft:golden_carrot",    "minecraft:obsidian",
    "minecraft:tnt",            "minecraft:end_crystal",      "minecraft:cobblestone",
    "minecraft:oak_planks",
};

}  // namespace

// =============================================================================
//  Armadura
// =============================================================================

ArmorHud::ArmorHud()
    : Module("armor_hud", "Armor", "Your armor (and what you hold) with its icon and durability.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.004f;
    l.y = 0.55f;
    l.anchorY = 0.5f;
    SetDefaultLayout(l);
}

const char* ArmorHud::IconSvg() const {
    return render::icons::kShield;
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
    // Qué piezas mostrar (si no tienes nada puesto, un ejemplo en el editor).
    static const char* const kSample[] = {"minecraft:diamond_helmet", "minecraft:diamond_chestplate",
                                          "minecraft:diamond_leggings", "minecraft:diamond_boots"};
    inv::Item shown[6];
    int count = 0;
    bool any = false;
    for (int i = 0; i < 6; ++i) {
        any |= has_[i];
    }
    for (int i = 0; i < 6; ++i) {
        if (has_[i]) {
            shown[count++] = items_[i];
        } else if (!any && frame.editing && i < 4) {
            std::snprintf(shown[count].id, sizeof(shown[count].id), "%s", kSample[i]);
            shown[count].count = 1;
            shown[count].maxDamage = 363;
            shown[count].damage = 60 * i;
            ++count;
        } else if (!hideEmpty_ && i < 4 && any) {
            shown[count++] = inv::Item{};  // lugar vacío
        }
    }
    if (count == 0) {
        return ImVec2(0, 0);
    }

    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const float icon = 16.0f * u;
    const float pad = boxes_ == 0 ? 0.0f : 3.0f * u;
    const float gap = 2.0f * u;
    const bool vertical = orientation_ == 0;
    ImDrawList* draw = frame.draw;

    // El texto de cada pieza (durabilidad o cantidad) y su ancho.
    char values[6][24];
    char names[6][64];
    float valueWidth = 0.0f;
    float nameWidth = 0.0f;
    for (int i = 0; i < count; ++i) {
        const inv::Item& it = shown[i];
        values[i][0] = 0;
        names[i][0] = 0;
        if (it.id[0] != 0 && value_ != 3 && value_ != 2) {
            if (it.maxDamage > 0) {
                const int left = it.maxDamage - it.damage;
                std::snprintf(values[i], sizeof(values[i]), value_ == 1 ? "%d%%" : "%d",
                              value_ == 1 ? left * 100 / it.maxDamage : left);
            } else if (it.count > 1) {
                std::snprintf(values[i], sizeof(values[i]), "%d", it.count);
            }
        }
        if (showNames_ && it.id[0] != 0) {
            inv::PrettyName(it.id, names[i], sizeof(names[i]));
        }
        valueWidth = std::max(valueWidth, rh::TextSize(m, values[i]).x);
        nameWidth = std::max(nameWidth, rh::TextSize(m, names[i]).x);
    }
    const float barW = value_ == 2 ? 20.0f * u : 0.0f;
    const float textBlock = (nameWidth > 0 ? nameWidth + 4.0f * u : 0.0f) + valueWidth + barW;

    // Tamaño de cada pieza.
    const float itemW = vertical ? icon + (textBlock > 0 ? 3.0f * u + textBlock : 0.0f) : std::max(icon, valueWidth + barW);
    const float itemH = vertical ? icon : icon + (valueWidth > 0 || barW > 0 ? m.capHeight + 3.0f * u : 0.0f);
    const float totalW = std::floor(vertical ? itemW + pad * 2.0f : (itemW + pad * 2.0f) * count + gap * (count - 1));
    const float totalH = std::floor(vertical ? (itemH + pad * 2.0f) * count + gap * (count - 1) : itemH + pad * 2.0f);
    const ImVec2 o = frame.pos;
    if (boxes_ == 2) {
        rh::Box(draw, m, o, ImVec2(o.x + totalW, o.y + totalH));
    }
    for (int i = 0; i < count; ++i) {
        const inv::Item& it = shown[i];
        const ImVec2 cell(vertical ? o.x : o.x + (itemW + pad * 2.0f + gap) * static_cast<float>(i),
                          vertical ? o.y + (itemH + pad * 2.0f + gap) * static_cast<float>(i) : o.y);
        if (boxes_ == 1) {
            rh::Box(draw, m, cell, ImVec2(cell.x + itemW + pad * 2.0f, cell.y + itemH + pad * 2.0f));
        }
        const ImVec2 c(cell.x + pad, cell.y + pad);
        if (it.id[0] == 0) {
            continue;  // lugar vacío
        }
        const float frac = Fraction(it);
        const ImU32 valueColor = colorByDurability_ && it.maxDamage > 0 ? DurabilityColor(frac) : rh::TextColor();
        const ImVec2 iconPos(vertical ? c.x : std::floor(c.x + (itemW - icon) * 0.5f), c.y);
        DrawItemIcon(draw, it.id, iconPos, icon, it.icon, it.color);
        if (vertical) {
            float x = c.x + icon + 3.0f * u;
            const float ty = c.y + (icon - m.capHeight) * 0.5f - m.capTop;
            if (names[i][0] != 0) {
                rh::Text(draw, m, ImVec2(x, ty), names[i], rh::TextColor());
                x += nameWidth + 4.0f * u;
            }
            if (barW > 0 && it.maxDamage > 0) {
                const float by = std::floor(c.y + icon * 0.5f - u);
                draw->AddRectFilled(ImVec2(x, by), ImVec2(x + barW, by + 2.0f * u), IM_COL32(0, 0, 0, 170));
                draw->AddRectFilled(ImVec2(x, by), ImVec2(x + std::floor(barW * frac), by + 2.0f * u), valueColor);
            } else if (values[i][0] != 0) {
                rh::Text(draw, m, ImVec2(x, ty), values[i], valueColor);
            }
        } else {
            const float ty = c.y + icon + 2.0f * u - m.capTop;
            if (barW > 0 && it.maxDamage > 0) {
                const float bx = std::floor(c.x + (itemW - barW) * 0.5f);
                const float by = c.y + icon + 2.0f * u;
                draw->AddRectFilled(ImVec2(bx, by), ImVec2(bx + barW, by + 2.0f * u), IM_COL32(0, 0, 0, 170));
                draw->AddRectFilled(ImVec2(bx, by), ImVec2(bx + std::floor(barW * frac), by + 2.0f * u), valueColor);
            } else if (values[i][0] != 0) {
                const float vw = rh::TextSize(m, values[i]).x;
                rh::Text(draw, m, ImVec2(std::floor(c.x + (itemW - vw) * 0.5f), ty), values[i], valueColor);
            }
        }
    }
    return ImVec2(totalW, totalH);
}

bool ArmorHud::DrawSettings() {
    bool changed = false;
    ui::Section("Layout");
    changed |= ui::Choice("Orientation", &orientation_, {"Vertical", "Horizontal"});
    changed |= ui::Choice("Show", &value_, {"Uses", "Percent", "Bar", "Icons only"});
    changed |= ui::Choice("Boxes", &boxes_, {"No box", "One per piece", "One for all"});
    changed |= ui::Toggle("Piece names", &showNames_);
    ui::Section("Content");
    changed |= ui::Toggle("What you hold", &showHeld_);
    changed |= ui::Toggle("Color by durability", &colorByDurability_, "(green to red)");
    changed |= ui::Toggle("No gap for empty slots", &hideEmpty_);
    if (changed) {
        lastRead_ = 0;
    }
    return changed;
}

void ArmorHud::SaveSettings(nlohmann::json& out) const {
    out["orientation"] = orientation_;
    out["value"] = value_;
    out["boxes"] = boxes_;
    out["showHeld"] = showHeld_;
    out["showNames"] = showNames_;
    out["colorByDurability"] = colorByDurability_;
    out["hideEmpty"] = hideEmpty_;
}

void ArmorHud::LoadSettings(const nlohmann::json& in) {
    orientation_ = std::clamp(in.value("orientation", 0), 0, 1);
    value_ = std::clamp(in.value("value", 0), 0, 3);
    boxes_ = std::clamp(in.value("boxes", 0), 0, 2);
    showHeld_ = in.value("showHeld", true);
    showNames_ = in.value("showNames", false);
    colorByDurability_ = in.value("colorByDurability", true);
    hideEmpty_ = in.value("hideEmpty", true);
}

// =============================================================================
//  Ítem en la mano
// =============================================================================

HeldItem::HeldItem()
    : Module("held_item", "Held item", "Name, count and durability of the item you have selected.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.5f;
    l.y = 0.86f;
    l.anchorX = 0.5f;
    l.anchorY = 1.0f;
    SetDefaultLayout(l);
}

const char* HeldItem::IconSvg() const {
    return render::icons::kSword;
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
    char text[96] = "";
    char name[64] = "";
    if (showName_) {
        inv::PrettyName(item.id, name, sizeof(name));
    }
    char value[32] = "";
    if (item.maxDamage > 0) {
        const int left = item.maxDamage - item.damage;
        if (value_ == 1) {
            std::snprintf(value, sizeof(value), "%d%%", left * 100 / item.maxDamage);
        } else if (value_ == 2) {
            std::snprintf(value, sizeof(value), "%d", left);
        } else {
            std::snprintf(value, sizeof(value), "%d/%d", left, item.maxDamage);
        }
    } else if (item.count > 1) {
        std::snprintf(value, sizeof(value), "x%d", item.count);
    }
    std::snprintf(text, sizeof(text), "%s%s%s", name, name[0] != 0 && value[0] != 0 ? "  " : "", value);

    const float icon = showIcon_ ? 16.0f * u : 0.0f;
    const float pad = 4.0f * u;
    const ImVec2 ts = rh::TextSize(m, text);
    const float width = std::floor(pad * 2.0f + icon + (icon > 0 && text[0] != 0 ? 4.0f * u : 0.0f) + ts.x);
    const float height = std::floor(std::max(icon, m.capHeight) + pad * 2.0f);
    const ImVec2 o = frame.pos;
    rh::Box(frame.draw, m, o, ImVec2(o.x + width, o.y + height));
    if (showIcon_) {
        DrawItemIcon(frame.draw, item.id, ImVec2(o.x + pad, o.y + pad), icon, item.icon, item.color);
    }
    rh::Text(frame.draw, m, ImVec2(o.x + pad + icon + (icon > 0 ? 4.0f * u : 0.0f), o.y + (height - m.capHeight) * 0.5f - m.capTop),
             text, rh::TextColor());
    if (!showBar_ || item.maxDamage <= 0) {
        return ImVec2(width, height);
    }
    // Barra de durabilidad debajo, del ancho de la caja.
    const float frac = Fraction(item);
    const ImVec2 b0(o.x, o.y + height);
    const ImVec2 b1(o.x + width, b0.y + 2.0f * u);
    frame.draw->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 160));
    frame.draw->AddRectFilled(b0, ImVec2(b0.x + std::floor(width * frac), b1.y), DurabilityColor(frac));
    return ImVec2(width, height + 2.0f * u);
}

bool HeldItem::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Icon", &showIcon_);
    changed |= ui::Toggle("Name", &showName_);
    changed |= ui::Choice("Durability", &value_, {"Uses / max", "Percent", "Uses only"});
    changed |= ui::Toggle("Durability bar", &showBar_);
    return changed;
}

void HeldItem::SaveSettings(nlohmann::json& out) const {
    out["showIcon"] = showIcon_;
    out["showName"] = showName_;
    out["showBar"] = showBar_;
    out["value"] = value_;
}

void HeldItem::LoadSettings(const nlohmann::json& in) {
    showIcon_ = in.value("showIcon", true);
    showName_ = in.value("showName", true);
    showBar_ = in.value("showBar", true);
    value_ = std::clamp(in.value("value", 0), 0, 2);
}

// =============================================================================
//  Contador de ítems
// =============================================================================

ItemCounter::ItemCounter()
    : Module("item_counter", "Item counter",
             "Counts the items you pick across your whole inventory, each in its own box with its icon.", Category::Hud) {
    HudLayout l;
    l.x = 0.004f;
    l.y = 0.25f;
    SetDefaultLayout(l);
    tracked_ = {"minecraft:arrow", "minecraft:ender_pearl", "minecraft:golden_apple", "minecraft:totem_of_undying"};
}

const char* ItemCounter::IconSvg() const {
    return render::icons::kPackage;
}

ImVec2 ItemCounter::OnRenderHud(const HudFrame& frame) {
    const int tracked = std::min(static_cast<int>(tracked_.size()), kMaxTracked);
    if (Every(lastRead_, 250.0)) {
        // Una sola pasada por los 36 lugares (y la mano secundaria).
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
    const bool vertical = orientation_ == 0;

    // Primero medimos (para que todas las cajas tengan el mismo ancho).
    char texts[kMaxTracked][80];
    float widths[kMaxTracked]{};
    bool visible[kMaxTracked]{};
    float maxW = 0.0f;
    for (int i = 0; i < tracked; ++i) {
        visible[i] = !(hideZero_ && counts_[i] == 0 && !frame.editing);
        if (!visible[i]) {
            continue;
        }
        char number[24];
        FormatCount(counts_[i], numberFormat_, number, sizeof(number));
        if (showNames_) {
            char name[64];
            inv::PrettyName(tracked_[static_cast<std::size_t>(i)].c_str(), name, sizeof(name));
            std::snprintf(texts[i], sizeof(texts[i]), "%s: %s", name, number);
        } else {
            std::snprintf(texts[i], sizeof(texts[i]), "%s", number);
        }
        widths[i] = std::floor(4.0f * u + icon + 4.0f * u + rh::TextSize(m, texts[i]).x + 6.0f * u);
        maxW = std::max(maxW, widths[i]);
    }
    float x = frame.pos.x;
    float y = frame.pos.y;
    float totalW = 0.0f;
    float totalH = 0.0f;
    for (int i = 0; i < tracked; ++i) {
        if (!visible[i]) {
            continue;
        }
        const float w = sameWidth_ && vertical ? maxW : widths[i];
        const ImVec2 min(x, y);
        const ImVec2 max(min.x + w, y + boxH);
        rh::Box(frame.draw, m, min, max);
        DrawItemIcon(frame.draw, tracked_[static_cast<std::size_t>(i)].c_str(),
                     ImVec2(min.x + 4.0f * u, std::floor(y + (boxH - icon) * 0.5f)), icon);
        rh::Text(frame.draw, m, ImVec2(min.x + 8.0f * u + icon, y + (boxH - m.capHeight) * 0.5f - m.capTop), texts[i],
                 rh::TextColor());
        if (vertical) {
            totalW = std::max(totalW, w);
            y += boxH + gap;
            totalH = y - gap - frame.pos.y;
        } else {
            x += w + gap;
            totalW = x - gap - frame.pos.x;
            totalH = boxH;
        }
    }
    return ImVec2(totalW, totalH);
}

bool ItemCounter::DrawSettings() {
    bool changed = false;
    const float fs = ImGui::GetFontSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 accent = theme::Accent(1.0f);

    // --- Selector: una cuadrícula de íconos. Clic = agregar o quitar. ------------
    ui::Section("Pick the items");
    const float cell = fs * 2.6f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, static_cast<int>(avail / (cell + fs * 0.35f)));
    int n = 0;
    for (const char* id : kPickerItems) {
        ImGui::PushID(id);
        if (n % perRow != 0) {
            ImGui::SameLine(0.0f, fs * 0.35f);
        }
        const auto it = std::find(tracked_.begin(), tracked_.end(), id);
        const bool selected = it != tracked_.end();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##item", ImVec2(cell, cell))) {
            if (selected) {
                tracked_.erase(it);
            } else if (static_cast<int>(tracked_.size()) < kMaxTracked) {
                tracked_.emplace_back(id);
            }
            sound::Play(sound::Effect::Click);
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 max(pos.x + cell, pos.y + cell);
        draw->AddRectFilled(pos, max, selected ? ImGui::ColorConvertFloat4ToU32(theme::Accent(0.30f))
                                              : IM_COL32(255, 255, 255, hovered ? 30 : 14),
                            fs * 0.35f);
        if (selected) {
            draw->AddRect(pos, max, ImGui::ColorConvertFloat4ToU32(accent), fs * 0.35f, 0, 1.6f);
        }
        const float pad = cell * 0.18f;
        DrawItemIcon(draw, id, ImVec2(pos.x + pad, pos.y + pad), cell - pad * 2.0f);
        if (hovered) {
            char name[64];
            inv::PrettyName(id, name, sizeof(name));
            ImGui::SetTooltip("%s%s", name, selected ? "  (click to remove)" : "  (click to add)");
        }
        ImGui::PopID();
        ++n;
    }

    // --- Los elegidos, en orden (con flechas para reordenar) ---------------------
    ui::Section("On screen (in this order)");
    int remove = -1;
    int moveUp = -1;
    for (int i = 0; i < static_cast<int>(tracked_.size()); ++i) {
        ImGui::PushID(1000 + i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(fs * 1.4f, fs * 1.4f));
        DrawItemIcon(draw, tracked_[static_cast<std::size_t>(i)].c_str(), pos, fs * 1.4f);
        ImGui::SameLine();
        char name[64];
        inv::PrettyName(tracked_[static_cast<std::size_t>(i)].c_str(), name, sizeof(name));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        ImGui::SameLine(avail - fs * 6.5f);
        if (i > 0 && ImGui::SmallButton("Move up")) {
            moveUp = i;
        }
        ImGui::SameLine(avail - fs * 3.2f);
        if (ImGui::SmallButton("Remove")) {
            remove = i;
        }
        ImGui::PopID();
    }
    if (moveUp > 0) {
        std::swap(tracked_[static_cast<std::size_t>(moveUp)], tracked_[static_cast<std::size_t>(moveUp - 1)]);
        changed = true;
    }
    if (remove >= 0) {
        tracked_.erase(tracked_.begin() + remove);
        changed = true;
    }
    if (tracked_.empty()) {
        ui::Hint("None yet: pick some above.");
    }
    if (static_cast<int>(tracked_.size()) < kMaxTracked) {
        ui::Text("Other item", custom_, sizeof(custom_), "its id: minecraft:...");
        if (ui::Button("Add", " ") && custom_[0] != 0) {
            std::string id = custom_;
            if (id.find(':') == std::string::npos) {
                id = "minecraft:" + id;
            }
            tracked_.push_back(id);
            custom_[0] = 0;
            changed = true;
        }
    }

    ui::Section("Layout");
    changed |= ui::Choice("Orientation", &orientation_, {"Vertical", "Horizontal"});
    changed |= ui::Choice("Numbers", &numberFormat_, {"1.000.000", "1M", "1000000"});
    changed |= ui::Toggle("Show the name", &showNames_);
    changed |= ui::Toggle("Same width boxes", &sameWidth_);
    changed |= ui::Toggle("Hide the ones at 0", &hideZero_);
    if (changed) {
        lastRead_ = 0;
    }
    return changed;
}

void ItemCounter::SaveSettings(nlohmann::json& out) const {
    out["items"] = tracked_;
    out["orientation"] = orientation_;
    out["numberFormat"] = numberFormat_;
    out["hideZero"] = hideZero_;
    out["showNames"] = showNames_;
    out["sameWidth"] = sameWidth_;
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
    orientation_ = std::clamp(in.value("orientation", 0), 0, 1);
    numberFormat_ = std::clamp(in.value("numberFormat", 0), 0, 2);
    hideZero_ = in.value("hideZero", false);
    showNames_ = in.value("showNames", false);
    sameWidth_ = in.value("sameWidth", true);
}

// =============================================================================
//  Aviso de durabilidad baja
// =============================================================================

DurabilityWarning::DurabilityWarning()
    : Module("durability_warning", "Low durability warning",
             "Warns you with a message and a sound when an item or armor piece is about to break.",
             Category::Utility) {}

const char* DurabilityWarning::IconSvg() const {
    return render::icons::kAlert;
}

void DurabilityWarning::OnTick() {
    // Lugar 0..3 = armadura, 4 = ítem en la mano.
    for (int piece = 0; piece < 5; ++piece) {
        if (piece < 4 && !armorToo_) {
            continue;
        }
        inv::Item item;
        const int slot = inv::SelectedSlot();
        const bool ok = piece < 4 ? inv::ReadArmor(piece, item) : (slot >= 0 && inv::ReadSlot(slot, item));
        char* warned = warned_[piece];
        if (!ok || item.maxDamage <= 0) {
            continue;
        }
        const int left = item.maxDamage - item.damage;
        const int percent = left * 100 / item.maxDamage;
        const bool low = percent <= threshold_;
        const bool same = std::strcmp(item.id, warned) == 0;
        if (low && !same) {
            // Avisar una vez por ítem (no cada 50 ms).
            char name[64];
            inv::PrettyName(item.id, name, sizeof(name));
            notify::Push(notify::Kind::Warning, TrFormat("{}: low durability ({} uses, {}%)", name, left, percent));
            if (sound_) {
                sound::Play(sound::Effect::ToggleOff);
            }
            std::snprintf(warned, 64, "%s", item.id);
        } else if (!low && same) {
            warned[0] = 0;  // lo reparaste o lo cambiaste: puede volver a avisar
        }
    }
}

bool DurabilityWarning::DrawSettings() {
    bool changed = false;
    changed |= ui::SliderInt("Warn below", &threshold_, 1, 50, "%d%%");
    changed |= ui::Toggle("Armor too", &armorToo_);
    changed |= ui::Toggle("With sound", &sound_);
    return changed;
}

void DurabilityWarning::SaveSettings(nlohmann::json& out) const {
    out["threshold"] = threshold_;
    out["sound"] = sound_;
    out["armorToo"] = armorToo_;
}

void DurabilityWarning::LoadSettings(const nlohmann::json& in) {
    threshold_ = std::clamp(in.value("threshold", 10), 1, 50);
    sound_ = in.value("sound", true);
    armorToo_ = in.value("armorToo", true);
}

}  // namespace luma::modules::hud
