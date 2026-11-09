// =============================================================================
//  menu.cpp - Menú de Luma
// =============================================================================
//  ImGui es una biblioteca "immediate mode": no se crean botones que viven
//  para siempre, sino que CADA FRAME se describe la interfaz desde cero:
//
//      if (ImGui::Button("Hola")) { ...esto corre el frame en que se pulsa... }
//
//  Diseño (dos paneles redondeados, módulos como tarjetas en grilla):
//
//    +------------+  +-----------------------------------------------+
//    |  (*) Luma  |  | [ Buscar...            ] [Perfil v] [Editar HUD]|
//    |            |  |  HUD ---------------------------------------- |
//    | CATEGORÍAS |  |  +-------------------+  +-------------------+ |
//    | [ Todos  ] |  |  | [M] Marca de agua |  | [R] Overlay de... | |
//    | [  HUD   ] |  |  |             (o--) |  |             (--o) | |
//    |   ...      |  |  +-------------------+  +-------------------+ |
//    | LUMA       |  |                                               |
//    | [  Tema  ] |  |   Clic en una tarjeta = sus opciones          |
//    +------------+  +-----------------------------------------------+
//
//  Gran parte se dibuja "a mano" con la ImDrawList (rectángulos redondeados,
//  círculos y texto): así logramos un estilo propio sin imágenes. Solo se
//  ejecuta con el menú abierto, así que no cuesta nada mientras juegas.
// =============================================================================
#include "render/menu.h"

#include <windows.h>
#include <shellapi.h>  // ShellExecuteW: abrir la carpeta de Luma

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImLerp, ImClamp

#include "core/config.h"
#include "core/keys.h"
#include "core/paths.h"
#include "core/version.h"
#include "modules/module_manager.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "render/notifications.h"
#include "render/sound.h"
#include "render/theme.h"

namespace luma::render::menu {

namespace {

using modules::Category;
using modules::Manager;
using modules::Module;
namespace pal = theme::palette;

// --- Páginas ------------------------------------------------------------------
enum class Page { Modules, Theme, Profiles, Settings, Diagnostics };

Page g_page = Page::Modules;
// Filtro de categoría en la página de módulos (Count = todas).
Category g_categoryFilter = Category::Count;
// Módulo cuya página de opciones está abierta (nullptr = la grilla).
Module* g_detail = nullptr;
char g_search[64] = "";

// --- Animaciones ------------------------------------------------------------------
std::int64_t g_openedAt = 0;
std::int64_t g_closedAt = 0;
std::atomic<bool> g_closing{false};  // el menú se cerró y se está desvaneciendo
std::int64_t g_pageChangedAt = 0;    // para la transición entre páginas
float g_indicatorY = -1.0f;          // posición animada del indicador de la barra lateral
float g_indicatorTarget = -1.0f;
float g_indicatorHeight = 0.0f;
ImGuiID g_lastHovered = 0;           // para el sonido al pasar el mouse

constexpr double kOpenMs = 200.0;
constexpr double kCloseMs = 150.0;
constexpr double kPageMs = 180.0;

double MsSince(std::int64_t ticks) {
    return perf::TicksToMs(perf::Now() - ticks);
}

// Cambiar de página (o entrar/salir de un módulo): animación + sonido.
void OnNavigate() {
    g_pageChangedAt = perf::Now();
    sound::Play(sound::Effect::Click);
}

// Sonido suave al empezar a pasar el mouse sobre el último elemento dibujado.
void HoverFeedback(bool hovered) {
    const ImGuiID id = ImGui::GetItemID();
    if (hovered && id != g_lastHovered) {
        g_lastHovered = id;
        sound::Play(sound::Effect::Hover);
    } else if (!hovered && id == g_lastHovered) {
        g_lastHovered = 0;
    }
}

// --- Captura de teclas ----------------------------------------------------------
enum class CaptureTarget { None, Module, MenuKey, UnloadKey };
CaptureTarget g_capture = CaptureTarget::None;
Module* g_captureModule = nullptr;

// --- Perfiles (lista guardada para no leer el disco cada frame) ----------------
std::vector<std::string> g_profiles;
bool g_profilesDirty = true;
char g_newProfile[33] = "";

// --- Utilidades -------------------------------------------------------------------

ImU32 Col(ImVec4 color, float alphaMultiplier = 1.0f) {
    color.w *= alphaMultiplier * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImVec4 Mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(ImLerp(a.x, b.x, t), ImLerp(a.y, b.y, t), ImLerp(a.z, b.z, t), ImLerp(a.w, b.w, t));
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float Rounding() {
    return theme::Get().rounding * theme::UiScale();
}

// Animación suave de un valor hacia "target", guardada por ID en el almacén
// de ImGui de la ventana actual (así cada tarjeta recuerda su animación).
float Animate(ImGuiID id, float target, float speed = 14.0f) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float value = storage->GetFloat(id, target);
    if (theme::Get().animations) {
        value = ImLerp(value, target, std::min(1.0f, ImGui::GetIO().DeltaTime * speed));
        if (std::fabs(value - target) < 0.001f) {
            value = target;
        }
    } else {
        value = target;
    }
    storage->SetFloat(id, value);
    return value;
}

// ¿"text" contiene "query", sin importar mayúsculas? (Solo letras ASCII.)
bool ContainsInsensitive(const std::string& text, const char* query) {
    if (query[0] == '\0') {
        return true;
    }
    const std::size_t queryLength = std::strlen(query);
    if (queryLength > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + queryLength <= text.size(); ++i) {
        std::size_t j = 0;
        while (j < queryLength &&
               std::tolower(static_cast<unsigned char>(text[i + j])) ==
                   std::tolower(static_cast<unsigned char>(query[j]))) {
            ++j;
        }
        if (j == queryLength) {
            return true;
        }
    }
    return false;
}

ImVec4 CategoryColor(Category category) {
    switch (category) {
        case Category::Hud:         return ImVec4(0.35f, 0.60f, 0.98f, 1.0f);
        case Category::Visual:      return ImVec4(0.70f, 0.48f, 0.98f, 1.0f);
        case Category::Utility:     return ImVec4(0.28f, 0.80f, 0.72f, 1.0f);
        case Category::World:       return ImVec4(0.45f, 0.80f, 0.35f, 1.0f);
        case Category::Performance: return ImVec4(0.98f, 0.62f, 0.25f, 1.0f);
        case Category::Count:       break;
    }
    return theme::Accent(1.0f);
}

const char* UpcomingPhase(Category category) {
    switch (category) {
        case Category::Hud:     return "FPS, CPS, keystrokes, reloj... llegan en la Fase 5.";
        case Category::Visual:  return "Zoom, FOV, fullbright... llegan en la Fase 8.";
        case Category::Utility: return "Toggle sprint, chat... llegan en las Fases 5 y 7.";
        case Category::World:   return "Free cam y minimapa llegan en las Fases 10 y 11.";
        default:                return "Llegan en las próximas fases.";
    }
}

// Texto centrado dentro de un rectángulo.
void CenteredText(ImDrawList* draw, ImFont* font, ImVec2 min, ImVec2 max, ImU32 color, const char* text) {
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
    draw->AddText(font, font->FontSize,
                  ImVec2(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f),
                  color, text);
}

// --- Widgets propios ----------------------------------------------------------

// Interruptor tipo "píldora": fondo oscuro con una perilla roja (apagado,
// a la izquierda) o verde (encendido, a la derecha). Devuelve true si cambió.
bool PillToggle(const char* id, bool* value, float height) {
    const float width = height * 2.3f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (clicked) {
        *value = !*value;
        sound::Play(*value ? sound::Effect::ToggleOn : sound::Effect::ToggleOff);
    }
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float t = Animate(ImGui::GetItemID(), *value ? 1.0f : 0.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = height * 0.28f;
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                        Col(hovered ? pal::kTileHover : pal::kPanelDark), r);
    const float pad = height * 0.14f;
    const float knobW = height - pad * 2.0f;
    const float x = pos.x + pad + t * (width - knobW - pad * 2.0f);
    const ImVec2 kMin(x, pos.y + pad);
    const ImVec2 kMax(x + knobW, pos.y + height - pad);
    draw->AddRectFilled(kMin, kMax, Col(Mix(pal::kOff, pal::kOn, t)), r * 0.8f);
    // Marca dentro de la perilla: punto (apagado) o barra (encendido).
    const ImVec2 c((kMin.x + kMax.x) * 0.5f, (kMin.y + kMax.y) * 0.5f);
    const ImU32 mark = Col(ImVec4(1, 1, 1, 0.92f));
    if (t > 0.5f) {
        const float half = knobW * 0.22f;
        draw->AddRectFilled(ImVec2(c.x - knobW * 0.06f, c.y - half), ImVec2(c.x + knobW * 0.06f, c.y + half),
                            mark, 1.0f);
    } else {
        draw->AddCircleFilled(c, knobW * 0.12f, mark);
    }
    return clicked;
}

// Botón grande de la barra lateral (texto centrado). Devuelve true al pulsarlo.
bool SidebarButton(const char* label, bool selected) {
    const float height = ImGui::GetFontSize() * 2.3f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 18.0f);
    const float active = Animate(ImGui::GetItemID() + 1, selected ? 1.0f : 0.0f, 12.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Al pasar el mouse, el botón se corre un poquito a la derecha.
    const float nudge = hover * ImGui::GetFontSize() * 0.15f;
    const ImVec2 min(pos.x + nudge, pos.y);
    const ImVec2 max(pos.x + width, pos.y + height);
    ImVec4 background = Mix(pal::kTile, pal::kTileHover, hover);
    background = Mix(background, Mix(pal::kPanelDark, theme::Accent(1.0f), 0.18f), active);
    draw->AddRectFilled(min, max, Col(background), Rounding() * 0.6f);
    if (active > 0.01f) {
        draw->AddRect(min, max, Col(theme::Accent(0.45f * active)), Rounding() * 0.6f, 0, 1.5f);
    }
    CenteredText(draw, ImGui::GetFont(), min, max, Col(Mix(pal::kText, theme::Accent(1.0f), active)), label);
    if (selected) {
        // El indicador lateral se desliza hasta el botón elegido (ver Sidebar).
        g_indicatorTarget = pos.y - ImGui::GetWindowPos().y;
        g_indicatorHeight = height;
    }
    return clicked;
}

void SidebarLabel(const char* text) {
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    const float textWidth = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - textWidth) * 0.5f);
    ImGui::TextDisabled("%s", text);
}

// Cuadrado de color con la inicial del módulo (el "ícono" de la tarjeta).
void ModuleIcon(ImDrawList* draw, ImVec2 min, float size, const Module& module) {
    const ImVec4 color = CategoryColor(module.GetCategory());
    const ImVec2 max(min.x + size, min.y + size);
    draw->AddRectFilled(min, max, Col(Mix(pal::kPanelDark, color, 0.35f)), size * 0.25f);
    draw->AddRect(min, max, Col(color, 0.8f), size * 0.25f, 0, 1.5f);
    const char initial[2] = {module.Name().empty() ? '?' : module.Name()[0], '\0'};
    CenteredText(draw, theme::GetFonts().medium, min, max, Col(color), initial);
}

// Botón que muestra una tecla y, al pulsarlo, espera la nueva.
// Clic derecho = quitar la tecla.
void KeybindButton(const char* id, int key, bool capturing, CaptureTarget target, Module* module) {
    const std::string label = capturing ? std::string("Pulsa una tecla...") : keys::Name(key);
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, capturing ? theme::Accent(0.55f) : pal::kPanelDark);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, capturing ? theme::Accent(0.65f) : pal::kTileHover);
    if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f))) {
        g_capture = capturing ? CaptureTarget::None : target;
        g_captureModule = capturing ? nullptr : module;
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clic: cambiar la tecla. Clic derecho: quitarla.\n"
                          "Mientras espera: Esc cancela, Retroceso la quita.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && target == CaptureTarget::Module &&
        module != nullptr) {
        module->SetKeybind(keys::kNone);
        config::MarkDirty();
    }
    ImGui::PopID();
}

// Botón con el color de acento (para acciones principales).
bool AccentButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent(0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Accent(1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::Accent(0.7f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// Título de página (fuente grande).
void PageTitle(const char* title, const char* subtitle) {
    ImGui::PushFont(theme::GetFonts().title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle != nullptr) {
        ImGui::TextDisabled("%s", subtitle);
    }
    ImGui::Spacing();
}

// Encabezado de sección: texto y una línea hasta el borde derecho.
void SectionHeader(const char* text) {
    ImGui::Spacing();
    ImGui::PushFont(theme::GetFonts().medium);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::TextColored(pal::kTextDim, "%s", text);
    const ImVec2 size = ImGui::GetItemRectSize();
    ImGui::PopFont();
    const float lineY = pos.y + size.y * 0.55f;
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x + size.x + ImGui::GetFontSize() * 0.8f, lineY),
                                        ImVec2(right, lineY), Col(ImVec4(1, 1, 1, 0.10f)), 1.0f);
}

// --- Tarjetas de módulos ----------------------------------------------------------

void ModuleTile(Module& module, float width) {
    ImGui::PushID(module.Id().c_str());
    const float fontSize = ImGui::GetFontSize();
    const float height = fontSize * 3.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + width, pos.y + height);

    // Zona del interruptor (derecha) separada del resto de la tarjeta.
    const float toggleHeight = height * 0.46f;
    const float toggleWidth = toggleHeight * 2.3f;
    const float toggleArea = toggleWidth + fontSize * 1.6f;

    // Toda la tarjeta (menos el interruptor) abre las opciones del módulo.
    const bool open = ImGui::InvisibleButton("##tile", ImVec2(width - toggleArea, height));
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 16.0f);
    const float on = Animate(ImGui::GetItemID() + 1, module.IsEnabled() ? 1.0f : 0.0f, 10.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = Rounding() * 0.7f;
    // Al pasar el mouse la tarjeta "se levanta": sube 2 px y su sombra crece.
    const float lift = hover * 2.0f;
    const ImVec2 tMin(pos.x, pos.y - lift);
    const ImVec2 tMax(max.x, max.y - lift);
    draw->AddRectFilled(ImVec2(tMin.x + 1, tMin.y + 2 + lift * 1.5f), ImVec2(tMax.x + 1, tMax.y + 2 + lift * 1.5f),
                        Col(ImVec4(0, 0, 0, 0.18f + 0.12f * hover)), r);
    draw->AddRectFilled(tMin, tMax, Col(Mix(pal::kTile, pal::kTileHover, hover)), r);
    if (on > 0.01f) {
        draw->AddRect(tMin, tMax, Col(pal::kOn, 0.40f * on), r, 0, 1.3f);
    }

    // Ícono y nombre (todo se dibuja desde "top", la parte de arriba ya levantada).
    const float top = tMin.y;
    const float iconSize = height * 0.56f;
    const ImVec2 iconMin(pos.x + height * 0.22f, top + (height - iconSize) * 0.5f);
    ModuleIcon(draw, iconMin, iconSize, module);
    ImFont* medium = theme::GetFonts().medium;
    const ImVec2 nameSize = medium->CalcTextSizeA(medium->FontSize, FLT_MAX, 0.0f, module.Name().c_str());
    const float textX = iconMin.x + iconSize + fontSize * 0.8f;
    draw->AddText(medium, medium->FontSize, ImVec2(textX, top + (height - nameSize.y) * 0.5f),
                  Col(pal::kText), module.Name().c_str());

    // Etiqueta con la tecla asignada (si tiene).
    if (module.Keybind() != keys::kNone) {
        const std::string key = keys::Name(module.Keybind());
        const ImVec2 keySize = ImGui::CalcTextSize(key.c_str());
        const ImVec2 kMin(textX + nameSize.x + fontSize * 0.6f, top + (height - keySize.y) * 0.5f - 2.0f);
        const ImVec2 kMax(kMin.x + keySize.x + fontSize * 0.7f, kMin.y + keySize.y + 4.0f);
        if (kMax.x < max.x - toggleArea) {
            draw->AddRectFilled(kMin, kMax, Col(pal::kPanelDark), 4.0f);
            draw->AddText(ImVec2(kMin.x + fontSize * 0.35f, kMin.y + 2.0f), Col(pal::kTextDim), key.c_str());
        }
    }

    // Separador vertical y el interruptor.
    const float dividerX = max.x - toggleArea;
    draw->AddLine(ImVec2(dividerX, top + height * 0.18f), ImVec2(dividerX, top + height * 0.82f),
                  Col(ImVec4(1, 1, 1, 0.08f)), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(dividerX + (toggleArea - toggleWidth) * 0.5f,
                                     top + (height - toggleHeight) * 0.5f));
    bool enabled = module.IsEnabled();
    if (PillToggle("##toggle", &enabled, toggleHeight)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }

    if (hovered) {
        ImGui::SetTooltip("%s\n\nClic para ver sus opciones.", module.Description().c_str());
    }
    if (open) {
        g_detail = &module;
        OnNavigate();
    }

    // Dejar el cursor debajo de la tarjeta (como si fuera un solo elemento).
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
}

// Grilla de tarjetas de una categoría. Devuelve cuántas dibujó.
int ModuleGrid(Category category, bool withHeader) {
    std::array<Module*, 64> matches{};
    std::size_t count = 0;
    for (const auto& module : Manager::All()) {
        if (module->GetCategory() != category || count == matches.size()) {
            continue;
        }
        if (ContainsInsensitive(module->Name(), g_search) ||
            ContainsInsensitive(module->Description(), g_search)) {
            matches[count++] = module.get();
        }
    }
    if (count == 0) {
        return 0;
    }
    if (withHeader) {
        SectionHeader(modules::CategoryName(category));
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = available > ImGui::GetFontSize() * 34.0f ? 2 : 1;
    const float width = (available - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (std::size_t i = 0; i < count; ++i) {
        if (i % static_cast<std::size_t>(columns) != 0) {
            ImGui::SameLine(0.0f, spacing);
        }
        ModuleTile(*matches[i], width);
    }
    return static_cast<int>(count);
}

// --- Páginas ------------------------------------------------------------------

void RefreshProfiles() {
    g_profiles = config::ListProfiles();
    g_profilesDirty = false;
}

void TopBar() {
    const float fontSize = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float comboWidth = fontSize * 11.0f;
    const float buttonWidth = fontSize * 9.5f;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fontSize * 0.75f, fontSize * 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Rounding() * 0.6f);

    // Búsqueda.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, pal::kField);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Mix(pal::kField, pal::kText, 0.06f));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - comboWidth - buttonWidth - spacing * 2.0f);
    ImGui::InputTextWithHint("##search", "Buscar...", g_search, sizeof(g_search));
    ImGui::PopStyleColor(2);

    // Perfil activo (cambiarlo desde aquí es lo más rápido).
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, pal::kPanelDark);
    const std::string active = config::ActiveProfile();
    if (ImGui::BeginCombo("##profile", active.c_str())) {
        if (ImGui::IsWindowAppearing() || g_profilesDirty) {
            RefreshProfiles();
        }
        std::string chosen;
        for (const std::string& name : g_profiles) {
            if (ImGui::Selectable(name.c_str(), name == active)) {
                chosen = name;
            }
        }
        ImGui::EndCombo();
        if (!chosen.empty() && config::SwitchProfile(chosen)) {
            theme::Apply();
            g_profilesDirty = true;
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Perfil activo");
    }

    // Editor de HUD (Fase 5).
    ImGui::SameLine();
    ImGui::BeginDisabled();
    AccentButton("Editar HUD", ImVec2(buttonWidth, 0.0f));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("El editor de HUD llega en la Fase 5.");
    }

    ImGui::PopStyleVar(2);
}

void DrawModulesPage() {
    TopBar();
    ImGui::Spacing();

    ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const bool searching = g_search[0] != '\0';
    int shown = 0;
    if (g_categoryFilter == Category::Count || searching) {
        // Todas las categorías, cada una con su encabezado.
        for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
            shown += ModuleGrid(static_cast<Category>(i), /*withHeader=*/true);
        }
    } else {
        SectionHeader(modules::CategoryName(g_categoryFilter));
        shown = ModuleGrid(g_categoryFilter, /*withHeader=*/false);
    }
    if (shown == 0) {
        ImGui::Spacing();
        if (searching) {
            ImGui::TextDisabled("Ningún módulo coincide con \"%s\".", g_search);
        } else {
            ImGui::TextDisabled("Todavía no hay módulos en esta categoría.");
            ImGui::TextDisabled("%s", UpcomingPhase(g_categoryFilter));
        }
    }
    ImGui::EndChild();
}

void DrawModuleDetail(Module& module) {
    const float fontSize = ImGui::GetFontSize();
    if (ImGui::Button("<  Volver")) {
        g_detail = nullptr;
        OnNavigate();
        return;
    }
    ImGui::Spacing();

    // Encabezado: ícono grande, nombre y categoría.
    const float iconSize = fontSize * 3.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ModuleIcon(ImGui::GetWindowDrawList(), pos, iconSize, module);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + iconSize + fontSize, pos.y));
    ImGui::BeginGroup();
    ImGui::PushFont(theme::GetFonts().title);
    ImGui::TextUnformatted(module.Name().c_str());
    ImGui::PopFont();
    ImGui::TextColored(CategoryColor(module.GetCategory()), "%s", modules::CategoryName(module.GetCategory()));
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, std::max(ImGui::GetCursorScreenPos().y, pos.y + iconSize) + fontSize * 0.5f));

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(module.Description().c_str());
    ImGui::PopTextWrapPos();

    SectionHeader("General");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Activado");
    ImGui::SameLine(fontSize * 10.0f);
    bool enabled = module.IsEnabled();
    if (PillToggle("##detail_toggle", &enabled, ImGui::GetFrameHeight() * 0.9f)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Tecla");
    ImGui::SameLine(fontSize * 10.0f);
    const bool capturing = g_capture == CaptureTarget::Module && g_captureModule == &module;
    KeybindButton("##detail_key", module.Keybind(), capturing, CaptureTarget::Module, &module);

    SectionHeader("Opciones");
    if (module.HasSettings()) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Rounding() * 0.5f);
        if (module.DrawSettings()) {
            config::MarkDirty();
        }
        ImGui::PopStyleVar();
    } else {
        ImGui::TextDisabled("Este módulo no tiene opciones.");
    }
}

void DrawThemePage() {
    PageTitle("Tema", "Cambia el aspecto del menú y del HUD.");
    theme::Settings& t = theme::Get();
    bool changed = false;

    SectionHeader("Colores");
    changed |= ImGui::ColorEdit3("Color de acento", t.accent, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    if (ImGui::SmallButton("Ámbar (por defecto)")) {
        const theme::Settings defaults;
        std::copy(std::begin(defaults.accent), std::end(defaults.accent), t.accent);
        changed = true;
    }
    changed |= ImGui::SliderFloat("Opacidad del menú", &t.windowOpacity, 0.5f, 1.0f, "%.2f");

    SectionHeader("Forma");
    changed |= ImGui::SliderFloat("Esquinas redondeadas", &t.rounding, 0.0f, 16.0f, "%.0f px");
    ImGui::SliderFloat("Tamaño de letra", &t.fontSize, 13.0f, 28.0f, "%.0f px");
    // La fuente se regenera al SOLTAR el slider (regenerarla cuesta unos ms).
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        theme::RequestFontRebuild();
        changed = true;
    }

    SectionHeader("Animaciones y fondo");
    changed |= ImGui::Checkbox("Animaciones suaves", &t.animations);
    ImGui::TextDisabled("Apertura y cierre, cambio de página, interruptores, tarjetas y avisos.");
    changed |= ImGui::SliderFloat("Oscurecer el juego detrás del menú", &t.backgroundDim, 0.0f, 0.8f, "%.2f");

    SectionHeader("Sonido");
    if (ImGui::Checkbox("Efectos de sonido", &t.sounds)) {
        changed = true;
        sound::Play(sound::Effect::Click);
    }
    ImGui::BeginDisabled(!t.sounds);
    ImGui::SliderFloat("Volumen", &t.soundVolume, 0.0f, 1.0f, "%.2f");
    // Los sonidos se regeneran al SOLTAR el slider (y suena uno de prueba).
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sound::SetVolume(t.soundVolume);
        sound::Play(sound::Effect::ToggleOn);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Probar")) {
        sound::Play(sound::Effect::Open);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Sonidos generados por Luma (sin archivos externos).");

    ImGui::Spacing();
    if (ImGui::Button("Restablecer tema")) {
        const float oldFont = t.fontSize;
        t = theme::Settings{};
        if (t.fontSize != oldFont) {
            theme::RequestFontRebuild();
        }
        changed = true;
    }
    if (changed) {
        theme::Apply();
        config::MarkDirty();
    }
}

void DrawProfilesPage() {
    if (g_profilesDirty) {
        RefreshProfiles();
    }
    PageTitle("Perfiles", "Un perfil guarda TODA la configuración: módulos, opciones, teclas y tema.");

    const std::string active = config::ActiveProfile();  // copia: puede cambiar abajo
    std::string toLoad;
    std::string toDelete;
    SectionHeader("Tus perfiles");
    const float fontSize = ImGui::GetFontSize();
    for (const std::string& name : g_profiles) {
        ImGui::PushID(name.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = fontSize * 2.4f;
        const bool isActive = name == active;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), Col(pal::kTile), Rounding() * 0.6f);
        if (isActive) {
            draw->AddRect(pos, ImVec2(pos.x + width, pos.y + height), Col(theme::Accent(0.6f)),
                          Rounding() * 0.6f, 0, 1.5f);
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x + fontSize, pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::AlignTextToFramePadding();
        if (isActive) {
            ImGui::TextColored(theme::Accent(1.0f), "%s", name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(activo)");
        } else {
            ImGui::TextUnformatted(name.c_str());
            ImGui::SameLine(width - fontSize * 9.5f);
            if (ImGui::Button("Cargar")) {
                toLoad = name;
            }
            ImGui::SameLine();
            if (ImGui::Button("Borrar")) {
                toDelete = name;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
    // Los cambios se aplican DESPUÉS de recorrer la lista.
    if (!toLoad.empty() && config::SwitchProfile(toLoad)) {
        theme::Apply();
        g_profilesDirty = true;
    }
    if (!toDelete.empty() && config::DeleteProfile(toDelete)) {
        g_profilesDirty = true;
    }

    SectionHeader("Nuevo perfil");
    ImGui::SetNextItemWidth(fontSize * 14.0f);
    ImGui::InputTextWithHint("##newprofile", "Nombre", g_newProfile, sizeof(g_newProfile));
    ImGui::SameLine();
    const bool valid = config::IsValidProfileName(g_newProfile);
    ImGui::BeginDisabled(!valid);
    if (AccentButton("Crear (copia del actual)", ImVec2(0.0f, 0.0f)) && config::CreateProfile(g_newProfile)) {
        g_newProfile[0] = '\0';
        g_profilesDirty = true;
    }
    ImGui::EndDisabled();
    if (!valid && g_newProfile[0] != '\0') {
        ImGui::TextDisabled("Solo letras, números, espacios, '-' y '_' (máx. 32).");
    }

    SectionHeader("Peligro");
    if (ImGui::Button("Restablecer este perfil...")) {
        ImGui::OpenPopup("##confirm_reset");
    }
    if (ImGui::BeginPopup("##confirm_reset")) {
        ImGui::Text("¿Volver todo el perfil \"%s\" a los valores de fábrica?", active.c_str());
        if (ImGui::Button("Sí, restablecer")) {
            config::ResetActiveProfile();
            theme::Apply();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawSettingsPage() {
    PageTitle("Ajustes", nullptr);
    const float labelWidth = ImGui::GetFontSize() * 14.0f;

    SectionHeader("Teclas de Luma");
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Abrir / cerrar el menú");
    ImGui::SameLine(labelWidth);
    KeybindButton("##menukey", config::g_menuKey.load(), g_capture == CaptureTarget::MenuKey,
                  CaptureTarget::MenuKey, nullptr);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Descargar Luma");
    ImGui::SameLine(labelWidth);
    KeybindButton("##unloadkey", config::g_unloadKey.load(), g_capture == CaptureTarget::UnloadKey,
                  CaptureTarget::UnloadKey, nullptr);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Limitación conocida: Luma todavía no sabe si el chat del juego está abierto, "
                        "así que si la tecla del menú es una letra, escribirla en el chat abre el "
                        "menú. Se corrige en la Fase 7. Una tecla como Insert o F8 evita el problema.");
    ImGui::PopTextWrapPos();

    SectionHeader("Archivos");
    if (ImGui::Button("Abrir la carpeta de Luma")) {
        // Abre %APPDATA%\Luma en el Explorador de archivos.
        ShellExecuteW(nullptr, L"open", paths::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::TextDisabled("Config, perfiles y logs.");
}

// Estadísticas calculadas a partir del historial de frames.
struct HistoryStats {
    float averageMs = 0.0f;
    float maxMs = 0.0f;
    float percentile99Ms = 0.0f;  // el frametime que solo supera el 1% más lento
    float openCostAvgUs = 0.0f;   // costo promedio con el menú abierto
    float openCostMaxUs = 0.0f;
};

HistoryStats ComputeStats(const perf::History& history) {
    HistoryStats stats;
    if (history.count == 0) {
        return stats;
    }
    // Copia en un arreglo de tamaño fijo (en la pila, sin pedir memoria) para
    // poder ordenarla sin desordenar el historial.
    std::array<float, perf::kHistorySize> sorted{};
    double sum = 0.0;
    double costSum = 0.0;
    std::size_t costCount = 0;
    for (std::size_t i = 0; i < history.count; ++i) {
        const float ms = history.frameMs[i];
        sorted[i] = ms;
        sum += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
        if (history.menuOpen[i]) {
            costSum += history.costUs[i];
            stats.openCostMaxUs = std::max(stats.openCostMaxUs, history.costUs[i]);
            ++costCount;
        }
    }
    stats.averageMs = static_cast<float>(sum / static_cast<double>(history.count));
    if (costCount > 0) {
        stats.openCostAvgUs = static_cast<float>(costSum / static_cast<double>(costCount));
    }
    // nth_element deja en la posición pedida el valor que estaría ahí si el
    // arreglo estuviera ordenado, sin ordenarlo entero (más rápido).
    const std::size_t index = (history.count * 99) / 100;
    auto end = sorted.begin() + static_cast<std::ptrdiff_t>(history.count);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), end);
    stats.percentile99Ms = sorted[index];
    return stats;
}

void DrawDiagnosticsPage(const RenderInfo& info) {
    PageTitle("Diagnóstico", "Rendimiento del juego y costo de Luma.");
    const perf::History& history = perf::GetHistory();
    const HistoryStats stats = ComputeStats(history);
    const float fps = stats.averageMs > 0.0f ? 1000.0f / stats.averageMs : 0.0f;
    const float low1 = stats.percentile99Ms > 0.0f ? 1000.0f / stats.percentile99Ms : 0.0f;

    SectionHeader("Frametime");
    ImGui::Text("FPS: %.0f   1%% low: %.0f   Frametime: %.2f ms (máx %.2f ms)", fps, low1,
                stats.averageMs, stats.maxMs);
    const float scaleMax = std::max(33.3f, stats.maxMs * 1.1f);
    // Si el historial ya dio la vuelta, "next" es el dato más viejo: el
    // gráfico empieza desde ahí para dibujarse en orden.
    const int offset = history.count == perf::kHistorySize ? static_cast<int>(history.next) : 0;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, pal::kPanelDark);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, theme::Accent(1.0f));
    ImGui::PlotLines("##frametime", history.frameMs.data(), static_cast<int>(history.count), offset,
                     nullptr, 0.0f, scaleMax,
                     ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 6.0f));
    ImGui::PopStyleColor(2);
    ImGui::TextDisabled("Últimos %zu frames. Más abajo = mejor; picos = tirones.", history.count);

    SectionHeader("Costo de Luma");
    const perf::Summary summary = perf::LastSummary();
    if (summary.valid && summary.closedFrames > 0) {
        // Criterio de las Fases 2 y 3: menos de 0,1 ms (100 µs) por frame con
        // el menú cerrado y sin módulos activos.
        const bool ok = summary.averageCostUs < 100.0;
        ImGui::Text("Menú cerrado (últimos %.0f s):", summary.seconds);
        ImGui::SameLine();
        ImGui::TextColored(ok ? pal::kOn : pal::kOff, "%.4f ms promedio", summary.averageCostUs / 1000.0);
        ImGui::Text("   máximo %.4f ms  ·  %llu frames", summary.maxCostUs / 1000.0,
                    static_cast<unsigned long long>(summary.closedFrames));
        ImGui::TextDisabled("Incluye lo que dibujen los módulos activos (HUD, avisos).");
    } else {
        ImGui::TextWrapped("Menú cerrado: todavía no hay datos. Deja el menú cerrado 30 s y "
                           "vuelve a abrirlo (el resumen también se escribe en el log).");
    }
    ImGui::Text("Menú abierto (ahora): %.3f ms promedio, %.3f ms máximo",
                stats.openCostAvgUs / 1000.0f, stats.openCostMaxUs / 1000.0f);

    SectionHeader("Render");
    ImGui::Text("DirectX 12  ·  %u x %u  ·  %s  ·  %u buffers", info.width, info.height,
                info.formatName, info.bufferCount);
}

// --- Barra lateral --------------------------------------------------------------

// Marca de Luma: un "sol" (círculo con rayos) y el nombre.
void Brand() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* title = theme::GetFonts().title;
    const float size = title->FontSize;
    const ImVec2 textSize = title->CalcTextSizeA(size, FLT_MAX, 0.0f, kName);
    const float iconSize = size * 0.9f;
    const float total = iconSize + size * 0.4f + textSize.x;
    const ImVec2 start(ImGui::GetCursorScreenPos().x + (ImGui::GetContentRegionAvail().x - total) * 0.5f,
                       ImGui::GetCursorScreenPos().y + size * 0.2f);

    const ImVec2 center(start.x + iconSize * 0.5f, start.y + textSize.y * 0.5f);
    const ImU32 accent = Col(theme::Accent(1.0f));
    draw->AddCircleFilled(center, iconSize * 0.24f, accent);
    for (int i = 0; i < 8; ++i) {
        const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
        const ImVec2 dir(std::cos(angle), std::sin(angle));
        draw->AddLine(ImVec2(center.x + dir.x * iconSize * 0.34f, center.y + dir.y * iconSize * 0.34f),
                      ImVec2(center.x + dir.x * iconSize * 0.48f, center.y + dir.y * iconSize * 0.48f),
                      accent, std::max(1.5f, size * 0.07f));
    }
    draw->AddText(title, size, ImVec2(start.x + iconSize + size * 0.4f, start.y), Col(pal::kText), kName);
    ImGui::Dummy(ImVec2(0.0f, textSize.y + size * 0.5f));
}

void Sidebar() {
    Brand();
    SidebarLabel("Categorías");
    g_indicatorTarget = -1.0f;
    auto categoryButton = [](const char* label, Category category) {
        const bool selected = g_page == Page::Modules && g_categoryFilter == category;
        if (SidebarButton(label, selected) && !(selected && g_detail == nullptr)) {
            g_page = Page::Modules;
            g_categoryFilter = category;
            g_detail = nullptr;
            OnNavigate();
        }
    };
    categoryButton("Todos", Category::Count);
    for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
        const auto category = static_cast<Category>(i);
        categoryButton(modules::CategoryName(category), category);
    }

    SidebarLabel("Luma");
    auto pageButton = [](const char* label, Page page) {
        if (SidebarButton(label, g_page == page) && g_page != page) {
            g_page = page;
            g_detail = nullptr;
            g_profilesDirty = true;
            OnNavigate();
        }
    };
    pageButton("Tema", Page::Theme);
    pageButton("Perfiles", Page::Profiles);
    pageButton("Ajustes", Page::Settings);
    pageButton("Diagnóstico", Page::Diagnostics);

    // Indicador: una barrita de color que se desliza hasta el botón elegido.
    if (g_indicatorTarget >= 0.0f) {
        if (g_indicatorY < 0.0f || !theme::Get().animations) {
            g_indicatorY = g_indicatorTarget;
        } else {
            g_indicatorY = ImLerp(g_indicatorY, g_indicatorTarget,
                                  std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f));
        }
        const ImVec2 win = ImGui::GetWindowPos();
        const float barWidth = std::max(3.0f, ImGui::GetFontSize() * 0.2f);
        const float inset = g_indicatorHeight * 0.22f;
        const float x = win.x + ImGui::GetStyle().WindowPadding.x * 0.35f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(x, win.y + g_indicatorY + inset),
            ImVec2(x + barWidth, win.y + g_indicatorY + g_indicatorHeight - inset),
            Col(theme::Accent(1.0f)), barWidth);
    }

    // Al pie: versión.
    const float footerY = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetFontSize() * 1.8f;
    if (ImGui::GetCursorScreenPos().y < footerY) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, footerY));
        SidebarLabel(kVersion);
    }
}

}  // namespace

void OnOpened() {
    g_openedAt = perf::Now();
    g_pageChangedAt = g_openedAt;
    g_closing.store(false);
    g_lastHovered = 0;
    sound::Play(sound::Effect::Open);
}

void OnClosed() {
    g_closedAt = perf::Now();
    // Sin animaciones, el menú desaparece al instante.
    g_closing.store(theme::Get().animations);
    sound::Play(sound::Effect::Close);
}

bool IsAnimatingClose() {
    return g_closing.load(std::memory_order_relaxed);
}

bool IsCapturingKey() {
    return g_capture != CaptureTarget::None;
}

void OnKeyCaptured(int vk) {
    const CaptureTarget target = g_capture;
    Module* module = g_captureModule;
    g_capture = CaptureTarget::None;
    g_captureModule = nullptr;

    if (vk == VK_ESCAPE) {
        return;  // cancelar
    }
    const bool clear = vk == VK_BACK || vk == VK_DELETE;
    if (!clear && !keys::IsBindable(vk)) {
        return;
    }
    switch (target) {
        case CaptureTarget::Module:
            if (module != nullptr) {
                module->SetKeybind(clear ? keys::kNone : vk);
            }
            break;
        case CaptureTarget::MenuKey:
            // La tecla del menú no se puede quitar (no habría forma de abrirlo).
            if (!clear) {
                config::g_menuKey.store(vk);
            }
            break;
        case CaptureTarget::UnloadKey:
            if (!clear) {
                config::g_unloadKey.store(vk);
            }
            break;
        case CaptureTarget::None:
            return;
    }
    if (!clear && vk == config::g_menuKey.load() && target == CaptureTarget::Module) {
        notify::Push(notify::Kind::Warning, "Esa tecla también abre el menú");
    }
    config::MarkDirty();
}

void Draw(const RenderInfo& info) {
    // --- Animación de apertura o de cierre ---------------------------------------
    // "appear" va de 0 (invisible) a 1 (completamente visible).
    float appear = 1.0f;
    const bool closing = g_closing.load(std::memory_order_relaxed);
    if (closing) {
        const float t = static_cast<float>(MsSince(g_closedAt) / kCloseMs);
        if (t >= 1.0f) {
            g_closing.store(false);  // terminó: el overlay vuelve al camino rápido
            return;
        }
        appear = 1.0f - t * t;  // "ease in": se va acelerando al desaparecer
    } else if (theme::Get().animations) {
        appear = EaseOut(static_cast<float>(MsSince(g_openedAt) / kOpenMs));
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fontSize = ImGui::GetFontSize();

    // Fondo: oscurecer el juego detrás del menú (barato: un rectángulo).
    const float dim = theme::Get().backgroundDim * appear;
    if (dim > 0.001f) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.02f, 0.02f, 0.04f, dim)));
    }

    // Ventana FIJA: siempre centrada y del mismo tamaño (no se puede mover ni
    // cambiar de tamaño). Se recalcula cada frame por si cambia la resolución.
    const ImVec2 size(std::max(viewport->Size.x * 0.66f, fontSize * 44.0f),
                      std::max(viewport->Size.y * 0.72f, fontSize * 26.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin(kName, nullptr, kFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 winSize = ImGui::GetWindowSize();
        // Al aparecer, el menú "sube" un poco hasta su lugar (y al cerrarse, baja).
        const float offsetY = (1.0f - appear) * fontSize * 1.8f;
        const float gap = fontSize * 1.0f;
        const float sidebarWidth = std::clamp(winSize.x * 0.2f, fontSize * 11.0f, fontSize * 15.0f);
        const float opacity = theme::Get().windowOpacity;
        const float r = Rounding() * 1.3f;

        // Los dos paneles (con una sombra suave debajo).
        const ImVec2 sideMin(winPos.x, winPos.y + offsetY);
        const ImVec2 sideMax(winPos.x + sidebarWidth, winPos.y + winSize.y + offsetY);
        const ImVec2 mainMin(sideMax.x + gap, sideMin.y);
        const ImVec2 mainMax(winPos.x + winSize.x, sideMax.y);
        for (const auto& [min, max] : {std::pair{sideMin, sideMax}, std::pair{mainMin, mainMax}}) {
            draw->AddRectFilled(ImVec2(min.x + 4, min.y + 6), ImVec2(max.x + 4, max.y + 6),
                                Col(ImVec4(0, 0, 0, 0.25f)), r);
        }
        draw->AddRectFilled(sideMin, sideMax, Col(pal::kPanelDark, opacity), r);
        draw->AddRectFilled(mainMin, mainMax, Col(pal::kPanel, opacity), r);
        draw->AddRect(mainMin, mainMax, Col(ImVec4(1, 1, 1, 0.05f)), r, 0, 1.0f);

        const float pad = fontSize * 1.1f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));

        // --- Barra lateral --------------------------------------------------------
        ImGui::SetCursorScreenPos(sideMin);
        ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, winSize.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        Sidebar();
        ImGui::EndChild();

        // --- Panel principal ------------------------------------------------------
        // Transición entre páginas: el contenido nuevo aparece y se desliza
        // un poco desde la derecha.
        float page = 1.0f;
        if (theme::Get().animations && !closing) {
            page = EaseOut(static_cast<float>(MsSince(g_pageChangedAt) / kPageMs));
        }
        ImGui::SetCursorScreenPos(ImVec2(mainMin.x + (1.0f - page) * fontSize * 1.6f, mainMin.y));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear * page);
        ImGui::BeginChild("##main", ImVec2(mainMax.x - mainMin.x, winSize.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        if (g_page == Page::Modules && g_detail != nullptr) {
            DrawModuleDetail(*g_detail);
        } else {
            switch (g_page) {
                case Page::Theme:       DrawThemePage(); break;
                case Page::Profiles:    DrawProfilesPage(); break;
                case Page::Settings:    DrawSettingsPage(); break;
                case Page::Diagnostics: DrawDiagnosticsPage(info); break;
                case Page::Modules:     DrawModulesPage(); break;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);  // alpha de la página + padding
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace luma::render::menu
