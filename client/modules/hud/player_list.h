// =============================================================================
//  player_list.h - Módulo "Lista de jugadores": la lista del Tab de Java
// =============================================================================
//  Mientras mantienes Tab (configurable) aparece arriba en el centro:
//    - el servidor (su dirección) o "Mundo local", cuántos jugadores hay y
//      tu ping;
//    - cada jugador con la cara de su skin y su nombre, y el logo de Luma
//      junto a quienes usan Luma (lo responde el servidor de Luma, ver
//      core/cloud.h).
//  El ping de los DEMÁS no se puede mostrar: en Bedrock el servidor no se lo
//  manda a tu juego (solo se sabe el tuyo).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "modules/module.h"
#include "sdk/players.h"

namespace luma::modules::hud {

class PlayerList : public Module {
public:
    PlayerList();
    const char* IconSvg() const override;
    bool HasHud() const override { return true; }
    bool IsHudElement() const override { return false; }  // no se mueve con el editor: va arriba al centro
    bool NeedsPlayer() const override { return true; }
    ImVec2 OnRenderHud(const HudFrame& frame) override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Refresh();
    int key_ = 0x09;           // VK_TAB
    bool showServer_ = true;
    bool showFaces_ = true;
    bool lumaFirst_ = false;   // los usuarios de Luma primero
    float size_ = 0.85f;       // tamaño de la lista (1 = el del HUD)
    std::vector<bool> rowOwn_;   // por fila (en el orden de players_): ¿eres tú?
    std::vector<bool> rowLuma_;  // ¿usa Luma?
    // Lo que se muestra (se relee 1 vez por segundo mientras está abierta).
    std::vector<sdk::players::Player> players_;
    std::unordered_map<std::string, sdk::players::Player> faces_;  // nombre -> cara (para no releerlas)
    sdk::players::Server server_;
    bool onServer_ = false;
    std::string ownName_;
    std::int64_t lastRefresh_ = 0;
    std::string title_;  // la dirección del servidor o el nombre del mundo
    bool logged_ = false;
    bool loggedFail_ = false;
};

}  // namespace luma::modules::hud
