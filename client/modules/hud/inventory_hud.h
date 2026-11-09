// =============================================================================
//  inventory_hud.h - Módulos con datos del inventario (Fase 7)
// =============================================================================
//  - Ítem en la mano: nombre, cantidad y durabilidad del ítem seleccionado.
//  - Contador de ítems: cuántos tienes en todo el inventario (del ítem en la
//    mano, o de uno fijo: flechas, tótems, perlas...).
//  - Aviso de durabilidad baja: un aviso (toast) y un sonido cuando el ítem
//    en la mano baja de un porcentaje. No dibuja nada: revisa cada 50 ms.
//
//  Datos: sdk/inventory.h (lecturas seguras). Se leen como mucho 4 veces por
//  segundo (no cada frame).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "modules/module.h"
#include "sdk/inventory.h"

namespace luma::modules::hud {

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
    bool showBar_ = true;      // barra de durabilidad
    bool percent_ = false;     // durabilidad en % en vez de "usos / máximo"
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
    static constexpr int kMaxTracked = 12;
    // Los ítems elegidos (ids "minecraft:...") y cuántos hay de cada uno.
    std::vector<std::string> tracked_;
    int counts_[kMaxTracked]{};
    bool hideZero_ = false;   // no mostrar los que tienes 0
    std::int64_t lastRead_ = 0;
    char custom_[64] = "";    // caja de texto para agregar un id a mano
};

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
    bool showHeld_ = true;     // también el ítem en la mano y la mano secundaria
    bool percent_ = false;
    sdk::inventory::Item items_[6];
    bool has_[6]{};
    std::int64_t lastRead_ = 0;
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
    int threshold_ = 10;  // porcentaje
    char warnedId_[64] = "";
    int warnedSlot_ = -1;
};

}  // namespace luma::modules::hud
