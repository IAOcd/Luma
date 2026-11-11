// =============================================================================
//  hud_editor.cpp - Implementación del editor de HUD
// =============================================================================
#include "render/hud_editor.h"
#include "core/tr.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "modules/module_manager.h"
#include "render/glass.h"
#include "render/module_options.h"
#include "render/ui.h"
#include "render/sound.h"
#include "render/theme.h"

namespace luma::render::hudeditor {

namespace {

using modules::HudLayout;
using modules::Manager;
using modules::Module;

bool g_active = false;
Module* g_dragging = nullptr;
ImVec2 g_grabOffset{0, 0};   // dónde agarraste el elemento (relativo a su esquina)
bool g_snapping = true;      // "imanes" activados
float g_guideX = -1.0f;      // línea guía vertical a dibujar (-1 = ninguna)
float g_guideY = -1.0f;
ImVec2 g_toolbarMin{0, 0};   // barra de herramientas (del frame anterior)
ImVec2 g_toolbarMax{0, 0};
Module* g_panel = nullptr;   // elemento con el panel de opciones abierto (clic derecho)
ImVec2 g_panelMin{0, 0};     // rectángulo del panel (del frame anterior)
ImVec2 g_panelMax{0, 0};

// CAMBIAR EL TAMAÑO DESDE UNA ESQUINA (como una ventana de Windows): al
// agarrar una esquina, la esquina OPUESTA queda fija y el tamaño sigue al
// mouse. Guardamos cómo estaba todo al empezar para calcular la proporción.
Module* g_resizing = nullptr;
int g_resizeCorner = 0;          // 0 arriba-izq, 1 arriba-der, 2 abajo-izq, 3 abajo-der
ImVec2 g_resizeFixed{0, 0};      // la esquina que no se mueve
ImVec2 g_resizeStartSize{1, 1};  // tamaño al empezar
float g_resizeStartScale = 1.0f;

// Las 4 esquinas de un elemento, en el orden de arriba.
ImVec2 Corner(const Module& m, int corner) {
    const ImVec2 p = m.LastPos();
    const ImVec2 s = m.LastSize();
    return ImVec2(p.x + ((corner & 1) != 0 ? s.x : 0.0f), p.y + ((corner & 2) != 0 ? s.y : 0.0f));
}

// ¿El mouse está sobre la esquina de algún elemento? (el de arriba primero)
Module* CornerHitTest(ImVec2 mouse, float radius, int& corner) {
    const auto& list = Manager::ActiveHud();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        Module* m = *it;
        if (!m->IsHudElement() || !m->CanScale()) {
            continue;
        }
        for (int c = 0; c < 4; ++c) {
            const ImVec2 p = Corner(*m, c);
            if (std::fabs(mouse.x - p.x) <= radius && std::fabs(mouse.y - p.y) <= radius) {
                corner = c;
                return m;
            }
        }
    }
    return nullptr;
}

bool Contains(ImVec2 min, ImVec2 size, ImVec2 p) {
    return p.x >= min.x && p.y >= min.y && p.x < min.x + size.x && p.y < min.y + size.y;
}

// El elemento bajo el mouse (el último dibujado está "arriba").
Module* HitTest(ImVec2 mouse) {
    const auto& list = Manager::ActiveHud();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        Module* m = *it;
        if (m->IsHudElement() && Contains(m->LastPos(), m->LastSize(), mouse)) {
            return m;
        }
    }
    return nullptr;
}

// IMANES: busca, entre todas las "líneas" candidatas (bordes y centro de la
// pantalla, bordes y centro de los demás elementos), la más cercana a alguno
// de los bordes o al centro del elemento. Si está a menos de "threshold"
// píxeles, mueve el elemento para que coincidan exactamente.
// Devuelve la línea usada (para dibujar la guía) o -1.
float SnapAxis(float& pos, float size, float screen, bool horizontal, float threshold) {
    // Los tres puntos del elemento: inicio, centro y fin.
    const float edges[3] = {pos, pos + size * 0.5f, pos + size};
    float best = threshold;
    float bestDelta = 0.0f;
    float bestLine = -1.0f;
    auto consider = [&](float line) {
        for (float e : edges) {
            const float d = line - e;
            if (std::fabs(d) < best) {
                best = std::fabs(d);
                bestDelta = d;
                bestLine = line;
            }
        }
    };
    consider(0.0f);
    consider(screen * 0.5f);
    consider(screen);
    for (Module* other : Manager::ActiveHud()) {
        if (other == g_dragging || !other->IsHudElement()) {
            continue;
        }
        const float start = horizontal ? other->LastPos().x : other->LastPos().y;
        const float length = horizontal ? other->LastSize().x : other->LastSize().y;
        consider(start);
        consider(start + length * 0.5f);
        consider(start + length);
    }
    if (bestLine >= 0.0f) {
        pos += bestDelta;
    }
    return bestLine;
}

// Guarda la posición en píxeles como posición relativa con su ancla.
void StoreLayout(Module& module, ImVec2 pos, ImVec2 size, ImVec2 display) {
    HudLayout& l = module.Layout();
    const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    // El ancla según el tercio de la pantalla en que quedó.
    auto anchor = [](float c, float screen) {
        return c < screen / 3.0f ? 0.0f : (c > screen * 2.0f / 3.0f ? 1.0f : 0.5f);
    };
    l.anchorX = anchor(center.x, display.x);
    l.anchorY = anchor(center.y, display.y);
    l.x = std::clamp((pos.x + l.anchorX * size.x) / display.x, 0.0f, 1.0f);
    l.y = std::clamp((pos.y + l.anchorY * size.y) / display.y, 0.0f, 1.0f);
}

ImU32 Col(ImVec4 c, float alpha = 1.0f) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

void DrawToolbar(ImVec2 display) {
    const float fs = ImGui::GetFontSize();
    const ImVec2 size(fs * 34.0f, fs * 3.3f);
    const ImVec2 pos(std::floor((display.x - size.x) * 0.5f), fs * 1.2f);
    g_toolbarMin = pos;
    g_toolbarMax = ImVec2(pos.x + size.x, pos.y + size.y);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.9f, fs * 0.55f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##hud_editor_toolbar", nullptr, kFlags)) {
        glass::Panel(ImGui::GetWindowDrawList(), g_toolbarMin, g_toolbarMax, theme::Get().rounding, 1.0f);
        ImGui::PushFont(theme::GetFonts().medium);
        ImGui::TextUnformatted(Tr("HUD editor"));
        ImGui::PopFont();
        ImGui::TextDisabled("%s", Tr("Drag to move · corners or wheel: size · right click: options · Shift: no snapping"));

        const float buttonsWidth = fs * 15.5f;
        ImGui::SetCursorPos(ImVec2(size.x - buttonsWidth - fs * 0.9f, (size.y - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Checkbox(Tr("Snapping"), &g_snapping)) {
            sound::Play(sound::Effect::Click);
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr("Reset"))) {
            for (Module* m : Manager::ActiveHud()) {
                Manager::ResetLayout(*m);
            }
            config::MarkDirty();
            sound::Play(sound::Effect::Click);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent(1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Accent(0.85f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::P().onAccentText);
        if (ImGui::Button(Tr("  Done  "))) {
            End();
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// El panel de opciones de un elemento: a la derecha de la pantalla (o a la
// izquierda, si el elemento está a la derecha, para no taparlo).
void DrawPanel(ImVec2 display) {
    if (g_panel == nullptr) {
        g_panelMin = g_panelMax = ImVec2(0, 0);
        return;
    }
    // Si el módulo se desactivó, ya no está en pantalla: cerrar el panel.
    bool stillActive = false;
    for (Module* m : Manager::ActiveHud()) {
        stillActive |= m == g_panel;
    }
    if (!stillActive) {
        g_panel = nullptr;
        return;
    }
    const float fs = ImGui::GetFontSize();
    const ImVec2 size(fs * 27.0f, std::min(display.y - fs * 8.0f, fs * 40.0f));
    const bool elementOnRight = g_panel->LastPos().x + g_panel->LastSize().x * 0.5f > display.x * 0.5f;
    const ImVec2 pos(elementOnRight ? fs * 1.2f : display.x - size.x - fs * 1.2f, fs * 6.0f);
    g_panelMin = pos;
    g_panelMax = ImVec2(pos.x + size.x, pos.y + size.y);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.0f, fs * 0.9f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##hud_editor_panel", nullptr, kFlags)) {
        glass::Panel(ImGui::GetWindowDrawList(), g_panelMin, g_panelMax, theme::Get().rounding, 1.0f);
        ImGui::PushFont(theme::GetFonts().medium);
        ImGui::TextUnformatted(g_panel->Name().c_str());
        ImGui::PopFont();
        ImGui::SameLine(size.x - fs * 3.2f);
        if (ImGui::Button(" X ")) {
            g_panel = nullptr;
            sound::Play(sound::Effect::Click);
        }
        if (g_panel != nullptr) {
            ImGui::BeginChild("##panel_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            ui::SetLabelColumn(11.0f);
            ImGui::PushItemWidth(-1.0f);
            if (moduleoptions::Draw(*g_panel)) {
                config::MarkDirty();
            }
            ImGui::PopItemWidth();
            ui::SetLabelColumn(15.0f);
            ImGui::EndChild();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

bool IsActive() {
    return g_active;
}

void Begin() {
    g_active = true;
    g_dragging = nullptr;
    g_resizing = nullptr;
    g_panel = nullptr;
    sound::Play(sound::Effect::Open);
}

void End() {
    if (!g_active) {
        return;
    }
    g_active = false;
    g_dragging = nullptr;
    g_resizing = nullptr;
    g_panel = nullptr;
    config::MarkDirty();
    // (El sonido lo pone el menú al volver a aparecer.)
}

void Draw() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const ImVec2 mouse = io.MousePos;
    const float threshold = 8.0f * theme::UiScale();
    auto inside = [&](ImVec2 a, ImVec2 b) { return mouse.x >= a.x && mouse.y >= a.y && mouse.x < b.x && mouse.y < b.y; };
    // Sobre la barra o el panel, el mouse es para ellos (no para mover elementos).
    const bool overToolbar = inside(g_toolbarMin, g_toolbarMax) || inside(g_panelMin, g_panelMax);

    // Ventana invisible que cubre la pantalla: así ImGui sabe que el mouse
    // es nuestro (y no hay clics "al vacío").
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    constexpr ImGuiWindowFlags kHostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##hud_editor", nullptr, kHostFlags);
    ImGui::End();

    // --- Input ---------------------------------------------------------------
    // Primero las esquinas (cambiar tamaño); si no, el elemento entero (mover).
    const float cornerRadius = 7.0f * theme::UiScale();
    int cornerUnderMouse = 0;
    Module* cornerHover = (g_dragging == nullptr && g_resizing == nullptr && !overToolbar)
                              ? CornerHitTest(mouse, cornerRadius, cornerUnderMouse)
                              : nullptr;
    if (cornerHover != nullptr && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_resizing = cornerHover;
        g_resizeCorner = cornerUnderMouse;
        g_resizeFixed = Corner(*cornerHover, 3 - cornerUnderMouse);  // la opuesta
        g_resizeStartSize = ImVec2(std::max(1.0f, cornerHover->LastSize().x), std::max(1.0f, cornerHover->LastSize().y));
        g_resizeStartScale = cornerHover->Layout().scale;
        sound::Play(sound::Effect::Click);
    }
    if (g_resizing != nullptr) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_resizing = nullptr;
            config::MarkDirty();
        } else {
            // Proporción = cuánto creció el rectángulo entre la esquina fija y
            // el mouse (el promedio de lo ancho y lo alto).
            const float rx = std::fabs(mouse.x - g_resizeFixed.x) / g_resizeStartSize.x;
            const float ry = std::fabs(mouse.y - g_resizeFixed.y) / g_resizeStartSize.y;
            float scale = g_resizeStartScale * (rx + ry) * 0.5f;
            // Saltos de 5% (con Shift, de 25%: los tamaños más nítidos de la letra pixelada).
            const float step = io.KeyShift ? 4.0f : 20.0f;
            scale = std::clamp(std::round(scale * step) / step, 0.5f, 4.0f);
            g_resizing->Layout().scale = scale;
            // El tamaño nuevo (proporcional) y dónde queda para que la esquina
            // opuesta no se mueva.
            const float k = scale / g_resizeStartScale;
            const ImVec2 size(g_resizeStartSize.x * k, g_resizeStartSize.y * k);
            const ImVec2 pos((g_resizeCorner & 1) != 0 ? g_resizeFixed.x : g_resizeFixed.x - size.x,
                             (g_resizeCorner & 2) != 0 ? g_resizeFixed.y : g_resizeFixed.y - size.y);
            StoreLayout(*g_resizing, pos, size, display);
        }
    }
    const int cursorCorner = g_resizing != nullptr ? g_resizeCorner : (cornerHover != nullptr ? cornerUnderMouse : -1);
    if (cursorCorner >= 0) {
        // Flecha diagonal: "\" para arriba-izq/abajo-der, "/" para las otras.
        ImGui::SetMouseCursor(cursorCorner == 0 || cursorCorner == 3 ? ImGuiMouseCursor_ResizeNWSE
                                                                      : ImGuiMouseCursor_ResizeNESW);
    }

    Module* hovered = g_resizing != nullptr ? g_resizing
                      : g_dragging != nullptr ? g_dragging
                      : (overToolbar ? nullptr : (cornerHover != nullptr ? cornerHover : HitTest(mouse)));
    if (g_dragging == nullptr && g_resizing == nullptr && cornerHover == nullptr && hovered != nullptr) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_dragging = hovered;
            g_grabOffset = ImVec2(mouse.x - hovered->LastPos().x, mouse.y - hovered->LastPos().y);
            sound::Play(sound::Effect::Click);
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Clic derecho: abrir (o cerrar) su panel de opciones.
            g_panel = g_panel == hovered ? nullptr : hovered;
            sound::Play(sound::Effect::Click);
        } else if (io.MouseWheel != 0.0f && hovered->CanScale()) {
            // Saltos de 0.25: con la letra pixelada, cada salto de 0.5 es un
            // tamaño nítido nuevo.
            float& scale = hovered->Layout().scale;
            scale = std::clamp(std::round((scale + io.MouseWheel * 0.25f) * 4.0f) / 4.0f, 0.5f, 4.0f);
            config::MarkDirty();
        }
    }
    g_guideX = -1.0f;
    g_guideY = -1.0f;
    if (g_dragging != nullptr) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_dragging = nullptr;
            config::MarkDirty();
        } else {
            const ImVec2 size = g_dragging->LastSize();
            ImVec2 pos(mouse.x - g_grabOffset.x, mouse.y - g_grabOffset.y);
            if (g_snapping && !io.KeyShift) {
                g_guideX = SnapAxis(pos.x, size.x, display.x, true, threshold);
                g_guideY = SnapAxis(pos.y, size.y, display.y, false, threshold);
            }
            pos.x = std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - size.x));
            pos.y = std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - size.y));
            StoreLayout(*g_dragging, pos, size, display);
        }
    }

    // --- Dibujo -----------------------------------------------------------------
    // En la capa de fondo, encima del HUD que ya se dibujó y debajo de la barra.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec4 accent = theme::Accent(1.0f);
    // Guías tenues del centro de la pantalla.
    draw->AddLine(ImVec2(display.x * 0.5f, 0), ImVec2(display.x * 0.5f, display.y), IM_COL32(255, 255, 255, 18));
    draw->AddLine(ImVec2(0, display.y * 0.5f), ImVec2(display.x, display.y * 0.5f), IM_COL32(255, 255, 255, 18));

    int count = 0;
    for (Module* m : Manager::ActiveHud()) {
        if (!m->IsHudElement()) {
            continue;
        }
        ++count;
        const ImVec2 min(m->LastPos().x - 2.0f, m->LastPos().y - 2.0f);
        const ImVec2 max(m->LastPos().x + m->LastSize().x + 2.0f, m->LastPos().y + m->LastSize().y + 2.0f);
        const bool active = m == hovered || m == g_panel;
        if (active) {
            draw->AddRectFilled(min, max, IM_COL32(255, 255, 255, 22));
        }
        draw->AddRect(min, max, active ? Col(accent) : IM_COL32(255, 255, 255, 90), 0.0f, 0, active ? 2.0f : 1.0f);
        if (active) {
            // Nombre y tamaño encima (o debajo, si no entra arriba).
            char label[96];
            if (m->CanScale()) {
                std::snprintf(label, sizeof(label), "%s  ·  %.0f%%", m->DisplayName(), m->Layout().scale * 100.0f);
            } else {
                std::snprintf(label, sizeof(label), "%s", m->DisplayName());
            }
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const float pad = ImGui::GetFontSize() * 0.35f;
            float ly = min.y - ts.y - pad * 2.0f - 2.0f;
            if (ly < 0.0f) {
                ly = max.y + 2.0f;
            }
            const float lx = std::clamp(min.x, 0.0f, std::max(0.0f, display.x - ts.x - pad * 2.0f));
            draw->AddRectFilled(ImVec2(lx, ly), ImVec2(lx + ts.x + pad * 2.0f, ly + ts.y + pad * 2.0f), Col(accent), pad);
            draw->AddText(ImVec2(lx + pad, ly + pad), Col(theme::P().onAccentText), label);
            // Las 4 esquinas: cuadraditos para agarrar y cambiar el tamaño.
            const float h = 3.0f * theme::UiScale();
            for (int c = 0; c < 4 && m->CanScale(); ++c) {
                const ImVec2 p = Corner(*m, c);
                draw->AddRectFilled(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), Col(accent));
                draw->AddRect(ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h), IM_COL32(255, 255, 255, 200));
            }
        }
    }
    if (g_guideX >= 0.0f) {
        draw->AddLine(ImVec2(g_guideX, 0), ImVec2(g_guideX, display.y), Col(accent, 0.8f), 1.5f);
    }
    if (g_guideY >= 0.0f) {
        draw->AddLine(ImVec2(0, g_guideY), ImVec2(display.x, g_guideY), Col(accent, 0.8f), 1.5f);
    }
    if (count == 0) {
        const char* text = Tr("No HUD elements are on. Turn some on (FPS, Keystrokes...) in the menu.");
        const ImVec2 ts = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2((display.x - ts.x) * 0.5f, display.y * 0.5f - ts.y), IM_COL32(255, 255, 255, 220), text);
    }

    DrawToolbar(display);
    DrawPanel(display);
}

}  // namespace luma::render::hudeditor
