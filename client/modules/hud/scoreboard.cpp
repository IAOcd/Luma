// =============================================================================
//  scoreboard.cpp - Scoreboard movible (ver scoreboard.h)
// =============================================================================
#include "modules/hud/scoreboard.h"

#include "render/icons.h"

#include <imgui.h>

#include "core/tr.h"
#include "render/ui.h"
#include "sdk/hud_ui.h"
#include "sdk/paperdoll.h"

namespace luma::modules::hud {

namespace ui = render::ui;

MovableScoreboard::MovableScoreboard()
    : Module("scoreboard", "Movable scoreboard",
             "Moves the scoreboard (the score table of servers and minigames) wherever you want with the HUD editor.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.82f;  // a la derecha, más o menos donde lo pone el juego
    l.y = 0.32f;
    SetDefaultLayout(l);
}

const char* MovableScoreboard::IconSvg() const {
    return render::icons::kTrophy;
}

bool MovableScoreboard::SdkReady() const {
    return sdk::paperdoll::Available();  // usa el mismo enganche de la interfaz del juego
}

void MovableScoreboard::OnEnable() {
    lastX_ = -1.0f;  // el próximo frame del HUD le pasa la posición al SDK
}

void MovableScoreboard::OnDisable() {
    sdk::hudui::SetScoreboard(false, lastX_, lastY_);  // vuelve a su lugar
}

ImVec2 MovableScoreboard::OnRenderHud(const HudFrame& frame) {
    // Píxeles de pantalla por unidad de la interfaz del juego (ver el muñeco).
    const float rootWidth = sdk::paperdoll::RootWidth();
    const float gui = rootWidth > 1.0f ? ImGui::GetIO().DisplaySize.x / rootWidth : 2.0f;
    const float x = frame.pos.x / gui;
    const float y = frame.pos.y / gui;
    if (x != lastX_ || y != lastY_) {
        lastX_ = x;
        lastY_ = y;
        sdk::hudui::SetScoreboard(true, x, y);
    }
    float w = sdk::hudui::ScoreboardWidth();
    float h = sdk::hudui::ScoreboardHeight();
    const bool seen = w > 1.0f && h > 1.0f;
    if (!seen) {
        w = 90.0f;  // tamaño de ejemplo mientras no hay ninguno en pantalla
        h = 80.0f;
    }
    const ImVec2 size(w * gui, h * gui);
    if (frame.editing && !seen) {
        // En el editor, sin scoreboard a la vista: una tabla de ejemplo.
        ImDrawList* d = frame.draw;
        const ImVec2 p = frame.pos;
        d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(0, 0, 0, 90));
        d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y * 0.18f), IM_COL32(0, 0, 0, 70));
        for (int i = 0; i < 4; ++i) {
            const float ry = p.y + size.y * (0.28f + i * 0.17f);
            d->AddRectFilled(ImVec2(p.x + size.x * 0.08f, ry), ImVec2(p.x + size.x * 0.6f, ry + size.y * 0.07f), IM_COL32(255, 255, 255, 60));
            d->AddRectFilled(ImVec2(p.x + size.x * 0.82f, ry), ImVec2(p.x + size.x * 0.92f, ry + size.y * 0.07f), IM_COL32(255, 120, 120, 90));
        }
        d->AddText(ImVec2(p.x, p.y + size.y + 2.0f), IM_COL32(255, 255, 255, 160), Tr("(shows up when a server displays one)"));
    }
    return size;
}

bool MovableScoreboard::DrawSettings() {
    ui::Hint("El scoreboard lo dibuja el juego: Luma solo lo mueve. Arrástralo desde el editor de HUD. También funciona "
             "con los scoreboards propios de algunos servidores.");
    return false;
}

}  // namespace luma::modules::hud
