// =============================================================================
//  paperdoll.cpp - Implementación del muñeco movible (ver paperdoll.h)
// =============================================================================
#include "modules/hud/paperdoll.h"

#include <imgui.h>

#include <algorithm>

#include "render/ui.h"
#include "sdk/paperdoll.h"

namespace luma::modules::hud {

namespace ui = render::ui;

// El muñeco del juego mide 15 unidades de ancho; lo que dibuja ocupa más o
// menos el doble de alto.
constexpr float kDollWidth = 15.0f;
constexpr float kDollHeight = 30.0f;

MovablePaperdoll::MovablePaperdoll()
    : Module("paperdoll", "Muñeco movible",
             "Mueve y agranda el muñeco de tu personaje que muestra el juego al correr, agacharte o volar.",
             Category::Hud) {
    HudLayout l;
    l.x = 0.02f;
    l.y = 0.04f;
    l.scale = 1.0f;
    SetDefaultLayout(l);
}

const char* MovablePaperdoll::IconSvg() const {
    // Una personita: cabeza (curvas, sin arcos), cuerpo, brazos y piernas.
    return "M12 2.5C13.4 2.5 14.5 3.6 14.5 5S13.4 7.5 12 7.5S9.5 6.4 9.5 5S10.6 2.5 12 2.5z "
           "M9 9h6v7H9z M9 10l-3 5 M15 10l3 5 M10.5 16v5.5 M13.5 16v5.5";
}

bool MovablePaperdoll::SdkReady() const {
    return sdk::paperdoll::Available();
}

void MovablePaperdoll::Push(bool enabled, float x, float y, float scale) const {
    sdk::paperdoll::Settings s;
    s.enabled = enabled;
    s.x = x;
    s.y = y;
    s.scale = scale;
    s.alwaysShow = alwaysShow_;
    sdk::paperdoll::Set(s);
}

void MovablePaperdoll::OnEnable() {
    lastScale_ = -1.0f;  // el próximo frame del HUD le pasa la posición al SDK
}

void MovablePaperdoll::OnDisable() {
    Push(false, lastX_, lastY_, lastScale_);  // el SDK lo devuelve a su lugar
}

ImVec2 MovablePaperdoll::OnRenderHud(const HudFrame& frame) {
    // Píxeles de pantalla por unidad de la interfaz del juego (la "escala de
    // la interfaz" de las opciones de video). Mientras no la sepamos, 2.
    const float rootWidth = sdk::paperdoll::RootWidth();
    const float display = ImGui::GetIO().DisplaySize.x;
    const float gui = rootWidth > 1.0f ? display / rootWidth : 2.0f;
    const ImVec2 size(kDollWidth * frame.scale * gui, kDollHeight * frame.scale * gui);

    // Solo avisar al SDK si cambió algo (no pedir el candado cada frame).
    const float x = frame.pos.x / gui;
    const float y = frame.pos.y / gui;
    if (x != lastX_ || y != lastY_ || frame.scale != lastScale_) {
        lastX_ = x;
        lastY_ = y;
        lastScale_ = frame.scale;
        Push(true, x, y, frame.scale);
    }

    if (frame.editing) {
        // En el editor: una silueta para ver dónde va a quedar (el juego solo
        // muestra el muñeco cuando corres, te agachas, nadas o vuelas).
        ImDrawList* d = frame.draw;
        const ImU32 fill = IM_COL32(255, 255, 255, 40);
        const float u = size.x / 8.0f;  // "píxel" de la silueta
        const ImVec2 p = frame.pos;
        auto rect = [&](float x0, float y0, float x1, float y1) {
            d->AddRectFilled(ImVec2(p.x + x0 * u, p.y + y0 * u), ImVec2(p.x + x1 * u, p.y + y1 * u), fill);
        };
        rect(2, 0, 6, 4);     // cabeza
        rect(2, 4, 6, 10);    // cuerpo
        rect(0, 4, 2, 10);    // brazo
        rect(6, 4, 8, 10);    // brazo
        rect(2, 10, 4, 16);   // pierna
        rect(4, 10, 6, 16);   // pierna
        if (!sdk::paperdoll::Found()) {
            d->AddText(ImVec2(p.x, p.y + size.y + 2.0f), IM_COL32(255, 255, 255, 160), "(corre para verlo)");
        }
    }
    return size;
}

bool MovablePaperdoll::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Siempre visible", &alwaysShow_, "(sin tener que correr ni agacharte)");
    ui::Hint("El muñeco lo dibuja el juego: Luma solo lo mueve y cambia su tamaño. Muévelo y agrándalo desde el "
             "editor de HUD.");
    if (changed) {
        Push(IsEnabled(), lastX_, lastY_, lastScale_);
    }
    return changed;
}

void MovablePaperdoll::SaveSettings(nlohmann::json& out) const {
    out["alwaysShow"] = alwaysShow_;
}

void MovablePaperdoll::LoadSettings(const nlohmann::json& in) {
    alwaysShow_ = in.value("alwaysShow", true);
}

}  // namespace luma::modules::hud
