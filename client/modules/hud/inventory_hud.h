// =============================================================================
//  inventory_hud.h - Módulos con datos del inventario (Fase 7)
// =============================================================================
//  - Armadura: tu armadura (y lo que tienes en las manos) con su ícono y su
//    durabilidad, al estilo de los clients de PvP.
//  - Ítem en la mano: nombre, cantidad y durabilidad del ítem seleccionado.
//  - Contador de ítems: los ítems que elijas, cada uno en su caja con su
//    ícono y cuántos tienes en todo el inventario.
//  - Aviso de durabilidad baja: un aviso (toast) y un sonido cuando el ítem
//    en la mano está por romperse. No dibuja nada: revisa cada 50 ms.
//
//  Datos: sdk/inventory.h (lecturas seguras), como mucho 4 veces por segundo.
//  Íconos: render/item_icons.h (leídos de tu instalación del juego).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "modules/module.h"
#include "sdk/inventory.h"

namespace luma::modules::hud {

class ArmorHud : public Module {
public:
    ArmorHud();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int orientation_ = 0;   // 0 = vertical, 1 = horizontal
    int value_ = 0;         // 0 = usos que quedan, 1 = porcentaje, 2 = barra, 3 = nada (solo íconos)
    int boxes_ = 0;         // 0 = sin caja, 1 = una por pieza, 2 = una para todo
    bool showHeld_ = true;  // también lo que tienes en las manos
    bool showNames_ = false;
    bool colorByDurability_ = true;
    bool hideEmpty_ = true;  // no dejar lugar para las piezas que no tienes puestas
    sdk::inventory::Item items_[6];
    bool has_[6]{};
    std::int64_t lastRead_ = 0;
};

class HeldItem : public Module {
public:
    HeldItem();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    bool showIcon_ = true;
    bool showName_ = true;
    bool showBar_ = true;
    int value_ = 0;  // 0 = usos / máximo, 1 = porcentaje, 2 = solo usos
    sdk::inventory::Item item_;
    bool hasItem_ = false;
    std::int64_t lastRead_ = 0;
};

class ItemCounter : public Module {
public:
    ItemCounter();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    static constexpr int kMaxTracked = 16;
    std::vector<std::string> tracked_;  // ids "minecraft:..."
    int counts_[kMaxTracked]{};
    int orientation_ = 0;     // 0 = vertical, 1 = horizontal
    int numberFormat_ = 0;    // 0 = 1.000.000, 1 = 1M, 2 = 1000000
    bool hideZero_ = false;
    bool showNames_ = false;
    bool sameWidth_ = true;   // todas las cajas del mismo ancho
    std::int64_t lastRead_ = 0;
    char custom_[64] = "";
};

class DurabilityWarning : public Module {
public:
    DurabilityWarning();
    const char* IconSvg() const override;
    bool NeedsPlayer() const override { return true; }
    bool HasTick() const override { return true; }
    void OnTick() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int threshold_ = 10;   // porcentaje
    bool sound_ = true;
    bool armorToo_ = true; // revisar también la armadura
    char warned_[6][64]{}; // por pieza: el ítem del que ya se avisó
};

}  // namespace luma::modules::hud
