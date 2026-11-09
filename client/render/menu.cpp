// =============================================================================
//  menu.cpp - Menú de Luma ("liquid glass")
// =============================================================================
//  ImGui es "immediate mode": CADA FRAME se describe la interfaz desde cero
//  (if (Boton("Hola")) {...}). ImGui recuerda posiciones y estados por dentro.
//
//  Diseño: dos paneles de vidrio flotando sobre el juego desenfocado.
//
//    +---------------+  +------------------------------------------------+
//    |  (L) Luma     |  |  Módulos                    [ (o) Buscar... ]  |
//    |               |  |  Todo lo que Luma puede hacer   [Perfil] [HUD] |
//    |  MÓDULOS      |  |                                                |
//    |  [#] Todos    |  |  HUD ─────────────────────────────────────     |
//    |  [ ] HUD      |  |  +-----------+  +-----------+  +-----------+   |
//    |  [ ] Visual   |  |  |  ( * )    |  |  ( ~ )    |  |           |   |
//    |   ...         |  |  |  Marca de |  |  Overlay  |  |           |   |
//    |  LUMA         |  |  |  agua  (o-)| |  ...  (-o)| |           |   |
//    |  [ ] Tema     |  |  +-----------+  +-----------+  +-----------+   |
//    +---------------+  +------------------------------------------------+
//
//  - Paneles: render/glass.h (el juego desenfocado + tinte + brillo + borde).
//  - Íconos: render/icons.h (paths SVG propios, dibujados como vectores).
//  - Logo: render/assets.h.
//  - Sonidos: render/sound.h.
//
//  Solo se ejecuta con el menú abierto: no cuesta nada mientras juegas.
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
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImLerp

#include "core/config.h"
#include "core/keys.h"
#include "core/paths.h"
#include "core/version.h"
#include "modules/module_manager.h"
#include "perf/benchmark.h"
#include "perf/frame_stats.h"
#include "perf/timer.h"
#include "perf/windows_info.h"
#include "render/assets.h"
#include "render/glass.h"
#include "render/hud.h"
#include "render/hud_editor.h"
#include "render/icons.h"
#include "render/input.h"
#include "render/notifications.h"
#include "render/sound.h"
#include "render/theme.h"

namespace luma::render::menu {

namespace {

using modules::Category;
using modules::Manager;
using modules::Module;

// =============================================================================
//  Estado
// =============================================================================

enum class Page { Modules, Theme, Profiles, Settings, Benchmark, Diagnostics };

Page g_page = Page::Modules;
Category g_categoryFilter = Category::Count;  // Count = todas
Module* g_detail = nullptr;                   // módulo con la página de opciones abierta
char g_search[64] = "";

// Animaciones.
std::int64_t g_openedAt = 0;
std::int64_t g_closedAt = 0;
std::atomic<bool> g_closing{false};
std::int64_t g_pageChangedAt = 0;
ImGuiID g_lastHovered = 0;
float g_navPillY = -1.0f;        // posición animada de la "píldora" de la barra lateral
float g_navPillTarget = -1.0f;
float g_navPillHeight = 0.0f;
float g_appear = 1.0f;           // 0..1 de la animación de apertura/cierre (para todo el frame)

constexpr double kOpenMs = 260.0;
constexpr double kCloseMs = 170.0;
constexpr double kPageMs = 220.0;

// Captura de teclas.
enum class CaptureTarget { None, Module, MenuKey, UnloadKey };
CaptureTarget g_capture = CaptureTarget::None;
Module* g_captureModule = nullptr;

// Perfiles (lista guardada para no leer el disco cada frame).
std::vector<std::string> g_profiles;
bool g_profilesDirty = true;
char g_newProfile[33] = "";

// Benchmark (página de Benchmark).
char g_benchLabel[40] = "";
int g_benchSeconds = 60;
int g_benchMode = 0;  // 0 = con mi HUD, 1 = Luma en reposo
perf::windows::Info g_windowsInfo;
bool g_windowsInfoDirty = true;

// =============================================================================
//  Utilidades
// =============================================================================

double MsSince(std::int64_t ticks) {
    return perf::TicksToMs(perf::Now() - ticks);
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// "Back out": se pasa un poquito del final y vuelve (efecto elástico suave).
float EaseOutBack(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float c1 = 1.25f;
    constexpr float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

float Fs() {
    return ImGui::GetFontSize();
}

float Rounding() {
    return theme::Get().rounding * theme::UiScale();
}

// Color con la transparencia del frame (animación de apertura) aplicada.
ImU32 Col(ImVec4 c, float alpha = 1.0f) {
    c.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImVec4 Mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(ImLerp(a.x, b.x, t), ImLerp(a.y, b.y, t), ImLerp(a.z, b.z, t), ImLerp(a.w, b.w, t));
}

// Valor animado hacia "target", guardado por ID en el almacén de ImGui.
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

void OnNavigate() {
    g_pageChangedAt = perf::Now();
    sound::Play(sound::Effect::Click);
}

void HoverFeedback(bool hovered) {
    const ImGuiID id = ImGui::GetItemID();
    if (hovered && id != g_lastHovered) {
        g_lastHovered = id;
        sound::Play(sound::Effect::Hover);
    } else if (!hovered && id == g_lastHovered) {
        g_lastHovered = 0;
    }
}

bool ContainsInsensitive(const std::string& text, const char* query) {
    if (query[0] == '\0') {
        return true;
    }
    const std::size_t n = std::strlen(query);
    if (n > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + n <= text.size(); ++i) {
        std::size_t j = 0;
        while (j < n && std::tolower(static_cast<unsigned char>(text[i + j])) ==
                            std::tolower(static_cast<unsigned char>(query[j]))) {
            ++j;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}

const char* CategoryIcon(Category category) {
    switch (category) {
        case Category::Hud:         return icons::kHud;
        case Category::Visual:      return icons::kVisual;
        case Category::Utility:     return icons::kUtility;
        case Category::World:       return icons::kWorld;
        case Category::Performance: return icons::kPerformance;
        case Category::Count:       break;
    }
    return icons::kAll;
}

const char* ModuleIcon(const Module& module) {
    const char* icon = module.IconSvg();
    return icon != nullptr ? icon : CategoryIcon(module.GetCategory());
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

void CenteredText(ImDrawList* draw, ImFont* font, ImVec2 min, ImVec2 max, ImU32 color, const char* text) {
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
    draw->AddText(font, font->FontSize,
                  ImVec2(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f),
                  color, text);
}

// =============================================================================
//  Widgets
// =============================================================================

// Interruptor estilo iOS: pista redondeada y perilla circular que se desliza.
bool Toggle(const char* id, bool* value, float height) {
    const float width = height * 1.75f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (clicked) {
        *value = !*value;
        sound::Play(*value ? sound::Effect::ToggleOn : sound::Effect::ToggleOff);
    }
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float t = Animate(ImGui::GetItemID(), *value ? 1.0f : 0.0f, 16.0f);
    const float press = Animate(ImGui::GetItemID() + 7, ImGui::IsItemActive() ? 1.0f : 0.0f, 20.0f);

    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + width, pos.y + height);
    const ImVec4 off = hovered ? theme::WithAlpha(p.trackOff, 1.5f) : p.trackOff;
    draw->AddRectFilled(pos, max, Col(Mix(off, theme::Accent(1.0f), t)), height * 0.5f);
    // Perilla: un círculo con una sombra suave; al apretarla se estira un poco.
    const float pad = height * 0.1f;
    const float r = height * 0.5f - pad;
    const float stretch = press * r * 0.35f;
    const float cx = pos.x + pad + r + t * (width - (pad + r) * 2.0f - stretch) + stretch * 0.5f * (1.0f - t);
    const float cy = pos.y + height * 0.5f;
    draw->AddRectFilled(ImVec2(cx - r, cy - r + 1.5f), ImVec2(cx + r + stretch, cy + r + 1.5f),
                        Col(ImVec4(0, 0, 0, 0.22f)), r);
    draw->AddRectFilled(ImVec2(cx - r, cy - r), ImVec2(cx + r + stretch, cy + r), Col(p.knob), r);
    return clicked;
}

// Botón de vidrio con ícono opcional. "primary" = relleno con el acento.
bool GlassButton(const char* label, const char* icon, ImVec2 size, bool primary = false) {
    const theme::Palette& p = theme::P();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float iconSize = Fs() * 1.05f;
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    if (size.x <= 0.0f) {
        size.x = textSize.x + style.FramePadding.x * 2.0f + (icon ? iconSize + style.ItemInnerSpacing.x : 0.0f);
    }
    if (size.y <= 0.0f) {
        size.y = ImGui::GetFrameHeight() * 1.1f;
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    HoverFeedback(hovered && !disabled);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 18.0f);
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = std::min(size.y * 0.5f, Rounding() * 0.7f);
    const float alpha = disabled ? 0.45f : 1.0f;
    ImVec4 textColor = p.text;
    if (primary) {
        draw->AddRectFilled(pos, max, Col(theme::Accent(0.88f + 0.12f * hover), alpha), r);
        textColor = p.onAccentText;
    } else {
        glass::Surface(draw, pos, max, r, alpha * ImGui::GetStyle().Alpha, hover);
    }
    float x = pos.x + (size.x - textSize.x - (icon ? iconSize + style.ItemInnerSpacing.x : 0.0f)) * 0.5f;
    if (icon != nullptr) {
        icons::Draw(draw, icon, ImVec2(x + iconSize * 0.5f, pos.y + size.y * 0.5f), iconSize, Col(textColor, alpha));
        x += iconSize + style.ItemInnerSpacing.x;
    }
    draw->AddText(ImVec2(x, pos.y + (size.y - textSize.y) * 0.5f), Col(textColor, alpha), label);
    if (clicked && !disabled) {
        sound::Play(sound::Effect::Click);
    }
    return clicked && !disabled;
}

// Control "segmentado": varias opciones en una píldora; la elegida resalta.
bool Segmented(const char* id, const char* const* labels, int count, int* index) {
    const float height = ImGui::GetFrameHeight() * 1.1f;
    const float width = Fs() * 7.0f * static_cast<float>(count);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##seg", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        const int clickedIndex = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - pos.x) /
                                                             (width / static_cast<float>(count))), 0, count - 1);
        if (clickedIndex != *index) {
            *index = clickedIndex;
            changed = true;
            sound::Play(sound::Effect::Click);
        }
    }
    const float t = Animate(ImGui::GetItemID(), static_cast<float>(*index), 16.0f);
    ImGui::PopID();

    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + width, pos.y + height);
    glass::Surface(draw, pos, max, height * 0.5f, ImGui::GetStyle().Alpha, hovered ? 0.4f : 0.0f);
    const float segment = width / static_cast<float>(count);
    const float pad = height * 0.1f;
    const ImVec2 pillMin(pos.x + segment * t + pad, pos.y + pad);
    const ImVec2 pillMax(pillMin.x + segment - pad * 2.0f, max.y - pad);
    draw->AddRectFilled(pillMin, pillMax, Col(theme::Accent(1.0f)), (height - pad * 2.0f) * 0.5f);
    for (int i = 0; i < count; ++i) {
        const ImVec2 sMin(pos.x + segment * static_cast<float>(i), pos.y);
        const ImVec2 sMax(sMin.x + segment, max.y);
        const float selected = std::clamp(1.0f - std::fabs(t - static_cast<float>(i)), 0.0f, 1.0f);
        CenteredText(draw, ImGui::GetFont(), sMin, sMax, Col(Mix(p.text, p.onAccentText, selected)), labels[i]);
    }
    return changed;
}

// Encabezado de sección: ícono, texto y una línea que se desvanece.
void SectionHeader(const char* icon, const char* text, int count = -1) {
    ImGui::Dummy(ImVec2(0.0f, Fs() * 0.4f));
    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float iconSize = Fs() * 1.1f;
    float x = pos.x;
    if (icon != nullptr) {
        icons::Draw(draw, icon, ImVec2(x + iconSize * 0.5f, pos.y + Fs() * 0.62f), iconSize, Col(p.textDim));
        x += iconSize + Fs() * 0.5f;
    }
    ImFont* medium = theme::GetFonts().medium;
    draw->AddText(medium, medium->FontSize, ImVec2(x, pos.y), Col(p.text), text);
    x += medium->CalcTextSizeA(medium->FontSize, FLT_MAX, 0.0f, text).x;
    if (count >= 0) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%d", count);
        draw->AddText(ImVec2(x + Fs() * 0.5f, pos.y + (medium->FontSize - Fs()) * 0.5f), Col(p.textDim), buffer);
        x += ImGui::CalcTextSize(buffer).x + Fs() * 0.5f;
    }
    const float lineY = pos.y + medium->FontSize * 0.55f;
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(ImVec2(x + Fs() * 0.8f, lineY), ImVec2(right, lineY + 1.0f), Col(p.border));
    // La línea se desvanece hacia la derecha (horizontal: usamos x en vez de y).
    for (int i = start; i < draw->VtxBuffer.Size; ++i) {
        ImDrawVert& v = draw->VtxBuffer[i];
        const float tt = std::clamp((v.pos.x - x) / std::max(1.0f, right - x), 0.0f, 1.0f);
        const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(a) * (1.0f - tt)) << IM_COL32_A_SHIFT);
    }
    ImGui::Dummy(ImVec2(0.0f, medium->FontSize + Fs() * 0.35f));
}

// Una "tarjeta" de vidrio con contenido normal de ImGui adentro (para grupos
// de opciones). Uso: if (BeginGroupCard("id")) {...} EndGroupCard();
struct GroupCard {
    ImVec2 start;  // esquina superior izquierda
    float width;   // ancho disponible al empezar
};
std::vector<GroupCard> g_groupStack;

void BeginGroupCard(const char* id) {
    ImGui::PushID(id);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Dos "canales": el fondo de la tarjeta se dibuja DESPUÉS (cuando sabemos
    // su alto) pero tiene que quedar DEBAJO del contenido.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    g_groupStack.push_back({start, ImGui::GetContentRegionAvail().x});
    const float pad = Fs() * 0.9f;
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, start.y + pad));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(std::max(Fs() * 8.0f, g_groupStack.back().width - pad * 2.0f - Fs() * 15.0f));
}

void EndGroupCard() {
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    const float pad = Fs() * 0.9f;
    const GroupCard card = g_groupStack.back();
    g_groupStack.pop_back();
    const ImVec2 end(card.start.x + card.width, ImGui::GetItemRectMax().y + pad);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSetCurrent(0);
    glass::Surface(draw, card.start, end, Rounding() * 0.8f, ImGui::GetStyle().Alpha, 0.0f);
    draw->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(card.start.x, end.y + Fs() * 0.6f));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

// Una fila "etiqueta ........ control" dentro de una tarjeta.
void RowLabel(const char* label, const char* hint = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (hint != nullptr) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::SameLine(Fs() * 15.0f);
}

void KeybindButton(const char* id, int key, bool capturing, CaptureTarget target, Module* module) {
    const theme::Palette& p = theme::P();
    const std::string label = capturing ? std::string("Pulsa una tecla...") : keys::Name(key);
    ImGui::PushID(id);
    const ImVec2 size(Fs() * 9.0f, ImGui::GetFrameHeight());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##key", size)) {
        g_capture = capturing ? CaptureTarget::None : target;
        g_captureModule = capturing ? nullptr : module;
        sound::Play(sound::Effect::Click);
    }
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    if (capturing) {
        // Parpadeo suave mientras espera la tecla.
        const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        draw->AddRectFilled(pos, max, Col(theme::Accent(0.35f + 0.35f * pulse)), size.y * 0.5f);
    } else {
        glass::Surface(draw, pos, max, size.y * 0.5f, ImGui::GetStyle().Alpha, hovered ? 1.0f : 0.0f);
    }
    icons::Draw(draw, icons::kKey, ImVec2(pos.x + size.y * 0.6f, pos.y + size.y * 0.5f), Fs(), Col(p.textDim));
    CenteredText(draw, ImGui::GetFont(), ImVec2(pos.x + size.y * 0.6f, pos.y), max,
                 Col(capturing ? p.onAccentText : p.text), label.c_str());
    if (hovered) {
        ImGui::SetTooltip("Clic: cambiar la tecla. Clic derecho: quitarla.\nMientras espera: Esc cancela, Retroceso la quita.");
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && target == CaptureTarget::Module && module != nullptr) {
        module->SetKeybind(keys::kNone);
        config::MarkDirty();
    }
    ImGui::PopID();
}

// Caja de búsqueda de vidrio con lupa.
void SearchField(float width) {
    const theme::Palette& p = theme::P();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight() * 1.1f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool active = ImGui::GetActiveID() == ImGui::GetID("##search");
    glass::Surface(draw, pos, ImVec2(pos.x + width, pos.y + height), height * 0.5f, ImGui::GetStyle().Alpha,
                   active ? 1.0f : 0.0f);
    icons::Draw(draw, icons::kSearch, ImVec2(pos.x + height * 0.55f, pos.y + height * 0.5f), Fs() * 1.05f,
                Col(p.textDim));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + height, pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::SetNextItemWidth(width - height * 1.3f);
    ImGui::InputTextWithHint("##search", "Buscar módulos...", g_search, sizeof(g_search));
    ImGui::PopStyleColor(3);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));
}

// =============================================================================
//  Tarjetas de módulos
// =============================================================================

void ModuleCard(Module& module, float width, float height, int index) {
    ImGui::PushID(module.Id().c_str());
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // Aparición escalonada: cada tarjeta entra un poco después que la anterior.
    float enter = 1.0f;
    if (theme::Get().animations) {
        enter = EaseOut(static_cast<float>((MsSince(g_pageChangedAt) - index * 35.0) / 260.0));
    }
    const float enterOffset = (1.0f - enter) * fs * 1.2f;

    const float toggleHeight = fs * 1.35f;
    const bool open = ImGui::InvisibleButton("##card", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 14.0f);
    const float on = Animate(ImGui::GetItemID() + 1, module.IsEnabled() ? 1.0f : 0.0f, 10.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = Rounding() * 0.85f;
    const float lift = hover * fs * 0.18f;
    const ImVec2 cMin(pos.x, pos.y - lift + enterOffset);
    const ImVec2 cMax(pos.x + width, pos.y + height - lift + enterOffset);
    const float alpha = enter * ImGui::GetStyle().Alpha;

    // Sombra que crece al levantarse.
    draw->AddRectFilled(ImVec2(cMin.x + 2, cMin.y + 4 + lift), ImVec2(cMax.x + 2, cMax.y + 4 + lift * 2.0f),
                        Col(p.shadow, (0.35f + 0.5f * hover) * enter), r);
    glass::Surface(draw, cMin, cMax, r, alpha, hover);
    if (on > 0.01f) {
        // Brillo del acento en el borde cuando está activo.
        draw->AddRect(cMin, cMax, Col(theme::Accent(0.55f * on), enter), r, 0, 1.6f);
    }

    // Ícono dentro de un círculo: con el acento si está activo.
    const float circle = fs * 3.1f;
    const ImVec2 iconCenter(cMin.x + fs * 1.1f + circle * 0.5f, cMin.y + fs * 1.1f + circle * 0.5f);
    draw->AddCircleFilled(iconCenter, circle * 0.5f, Col(Mix(p.field, theme::Accent(1.0f), on), enter), 40);
    draw->AddCircle(iconCenter, circle * 0.5f, Col(p.border, enter * (1.0f - on)), 40, 1.0f);
    icons::Draw(draw, ModuleIcon(module), iconCenter, fs * 1.6f, Col(Mix(p.text, p.onAccentText, on), enter));

    // Tecla asignada, arriba a la derecha.
    if (module.Keybind() != keys::kNone) {
        const std::string key = keys::Name(module.Keybind());
        const ImVec2 ks = ImGui::CalcTextSize(key.c_str());
        const ImVec2 kMax(cMax.x - fs * 0.9f, cMin.y + fs * 0.9f + ks.y + fs * 0.35f);
        const ImVec2 kMin(kMax.x - ks.x - fs * 0.9f, cMin.y + fs * 0.9f);
        draw->AddRectFilled(kMin, kMax, Col(p.field, enter), (kMax.y - kMin.y) * 0.5f);
        draw->AddText(ImVec2(kMin.x + fs * 0.45f, kMin.y + fs * 0.17f), Col(p.textDim, enter), key.c_str());
    }

    // Nombre y categoría.
    ImFont* medium = theme::GetFonts().medium;
    const float textY = iconCenter.y + circle * 0.5f + fs * 0.7f;
    draw->PushClipRect(cMin, ImVec2(cMax.x - fs * 0.6f, cMax.y), true);
    draw->AddText(medium, medium->FontSize, ImVec2(cMin.x + fs * 1.1f, textY), Col(p.text, enter), module.Name().c_str());
    draw->AddText(ImVec2(cMin.x + fs * 1.1f, textY + medium->FontSize + fs * 0.15f), Col(p.textDim, enter),
                  hover > 0.5f ? "Ver opciones  »" : modules::CategoryName(module.GetCategory()));
    draw->PopClipRect();

    // Interruptor, abajo a la derecha (un botón aparte sobre la tarjeta).
    const float toggleWidth = toggleHeight * 1.75f;
    ImGui::SetCursorScreenPos(ImVec2(cMax.x - fs * 1.0f - toggleWidth, cMax.y - fs * 1.0f - toggleHeight));
    bool enabled = module.IsEnabled();
    if (Toggle("##toggle", &enabled, toggleHeight)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }

    if (hovered && !ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", module.Description().c_str());
    }
    if (open) {
        g_detail = &module;
        OnNavigate();
    }

    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
}

// Grilla de una categoría. Devuelve cuántas tarjetas dibujó. "index" lleva la
// cuenta global para la aparición escalonada.
int ModuleGrid(Category category, bool withHeader, int& index) {
    std::array<Module*, 64> matches{};
    std::size_t count = 0;
    for (const auto& module : Manager::All()) {
        if (module->GetCategory() != category || count == matches.size()) {
            continue;
        }
        if (ContainsInsensitive(module->Name(), g_search) || ContainsInsensitive(module->Description(), g_search)) {
            matches[count++] = module.get();
        }
    }
    if (count == 0) {
        return 0;
    }
    if (withHeader) {
        SectionHeader(CategoryIcon(category), modules::CategoryName(category), static_cast<int>(count));
    }
    const float fs = Fs();
    const float gap = fs * 0.9f;
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((available + gap) / (fs * 13.5f + gap)));
    const float width = (available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float height = fs * 9.6f;
    for (std::size_t i = 0; i < count; ++i) {
        if (i % static_cast<std::size_t>(columns) != 0) {
            ImGui::SameLine(0.0f, gap);
        }
        ModuleCard(*matches[i], width, height, index++);
        if ((i + 1) % static_cast<std::size_t>(columns) == 0) {
            ImGui::Dummy(ImVec2(0.0f, gap - ImGui::GetStyle().ItemSpacing.y));
        }
    }
    return static_cast<int>(count);
}

// =============================================================================
//  Páginas
// =============================================================================

void PageHeader(const char* title, const char* subtitle) {
    ImGui::PushFont(theme::GetFonts().title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle != nullptr) {
        ImGui::TextDisabled("%s", subtitle);
    }
}

void RefreshProfiles() {
    g_profiles = config::ListProfiles();
    g_profilesDirty = false;
}

void ProfileCombo(float width) {
    const std::string active = config::ActiveProfile();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Fs() * 0.8f, ImGui::GetStyle().FramePadding.y * 1.2f));
    const std::string preview = "Perfil: " + active;
    if (ImGui::BeginCombo("##profile", preview.c_str())) {
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
            sound::Play(sound::Effect::Click);
        }
    }
    ImGui::PopStyleVar(2);
}

void DrawModulesPage() {
    const float fs = Fs();
    // Encabezado: título a la izquierda; búsqueda, perfil y HUD a la derecha.
    const float headerY = ImGui::GetCursorPosY();
    const float contentRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const char* title = g_categoryFilter == Category::Count ? "Módulos" : modules::CategoryName(g_categoryFilter);
    PageHeader(title, g_categoryFilter == Category::Count ? "Todo lo que Luma puede hacer."
                                                         : "Módulos de esta categoría.");
    const float afterHeader = ImGui::GetCursorPosY();
    const float rightWidth = std::min(ImGui::GetContentRegionAvail().x * 0.62f, fs * 34.0f);
    const float searchWidth = rightWidth - fs * 21.0f;
    ImGui::SetCursorPos(ImVec2(contentRight - rightWidth, headerY + fs * 0.25f));
    SearchField(std::max(fs * 9.0f, searchWidth));
    ImGui::SetCursorPos(ImVec2(contentRight - rightWidth + std::max(fs * 9.0f, searchWidth) + fs * 0.6f,
                               headerY + fs * 0.25f));
    ProfileCombo(fs * 10.0f);
    ImGui::SameLine(0.0f, fs * 0.6f);
    if (GlassButton("Editar HUD", icons::kMove, ImVec2(fs * 9.4f, ImGui::GetFrameHeight() * 1.1f), true)) {
        hudeditor::Begin();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Mover y escalar los elementos del HUD activos.");
    }
    ImGui::SetCursorPosY(std::max(afterHeader, headerY + ImGui::GetFrameHeight() * 1.1f + fs * 0.25f) + fs * 0.4f);

    ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const bool searching = g_search[0] != '\0';
    int shown = 0;
    int index = 0;
    if (g_categoryFilter == Category::Count || searching) {
        for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
            shown += ModuleGrid(static_cast<Category>(i), true, index);
        }
    } else {
        shown = ModuleGrid(g_categoryFilter, false, index);
    }
    if (shown == 0) {
        // Estado vacío: ícono grande y un texto amable.
        const theme::Palette& p = theme::P();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 center(ImGui::GetCursorScreenPos().x + avail.x * 0.5f, ImGui::GetCursorScreenPos().y + fs * 5.0f);
        icons::Draw(draw, searching ? icons::kSearch : CategoryIcon(g_categoryFilter), center, fs * 3.5f,
                    Col(p.textDim), fs * 0.16f);
        const char* line1 = searching ? "Ningún módulo coincide" : "Todavía no hay módulos aquí";
        const char* line2 = searching ? "Prueba con otra palabra." : UpcomingPhase(g_categoryFilter);
        ImFont* medium = theme::GetFonts().medium;
        CenteredText(draw, medium, ImVec2(center.x - avail.x * 0.5f, center.y + fs * 2.6f),
                     ImVec2(center.x + avail.x * 0.5f, center.y + fs * 4.0f), Col(p.text), line1);
        CenteredText(draw, ImGui::GetFont(), ImVec2(center.x - avail.x * 0.5f, center.y + fs * 4.0f),
                     ImVec2(center.x + avail.x * 0.5f, center.y + fs * 5.2f), Col(p.textDim), line2);
        ImGui::Dummy(ImVec2(0, fs * 11.0f));
    }
    ImGui::EndChild();
}

void DrawModuleDetail(Module& module) {
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    if (GlassButton("Volver", icons::kBack, ImVec2(0, 0))) {
        g_detail = nullptr;
        OnNavigate();
        return;
    }
    ImGui::Dummy(ImVec2(0, fs * 0.4f));

    // Encabezado grande: ícono en círculo, nombre, categoría.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float circle = fs * 4.6f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float on = Animate(ImGui::GetID("##hero"), module.IsEnabled() ? 1.0f : 0.0f, 10.0f);
    const ImVec2 center(pos.x + circle * 0.5f, pos.y + circle * 0.5f);
    draw->AddCircleFilled(center, circle * 0.5f, Col(Mix(p.field, theme::Accent(1.0f), on)), 48);
    icons::Draw(draw, ModuleIcon(module), center, fs * 2.3f, Col(Mix(p.text, p.onAccentText, on)));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + circle + fs, pos.y + fs * 0.4f));
    ImGui::BeginGroup();
    ImGui::PushFont(theme::GetFonts().title);
    ImGui::TextUnformatted(module.Name().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s", modules::CategoryName(module.GetCategory()));
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, std::max(ImGui::GetCursorScreenPos().y, pos.y + circle) + fs * 0.8f));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(module.Description().c_str());
    ImGui::PopTextWrapPos();

    SectionHeader(icons::kSettings, "General");
    BeginGroupCard("general");
    RowLabel("Activado");
    bool enabled = module.IsEnabled();
    if (Toggle("##detail_toggle", &enabled, ImGui::GetFrameHeight() * 0.95f)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }
    RowLabel("Tecla");
    const bool capturing = g_capture == CaptureTarget::Module && g_captureModule == &module;
    KeybindButton("##detail_key", module.Keybind(), capturing, CaptureTarget::Module, &module);
    EndGroupCard();

    SectionHeader(icons::kUtility, "Opciones");
    BeginGroupCard("options");
    if (module.HasSettings()) {
        if (module.DrawSettings()) {
            config::MarkDirty();
        }
    } else {
        ImGui::TextDisabled("Este módulo no tiene opciones.");
    }
    EndGroupCard();
}

void DrawThemePage() {
    PageHeader("Tema", "El aspecto de Luma: vidrio, colores, forma y sonido.");
    theme::Settings& t = theme::Get();
    bool changed = false;

    SectionHeader(icons::kTheme, "Estilo");
    BeginGroupCard("style");
    RowLabel("Vidrio");
    static const char* const kStyles[] = {"Oscuro", "Claro"};
    int style = t.style == theme::Style::Light ? 1 : 0;
    if (Segmented("style", kStyles, 2, &style)) {
        t.style = style == 1 ? theme::Style::Light : theme::Style::Dark;
        changed = true;
    }
    RowLabel("Colores de Luma", "(carbón y marfil)");
    if (Toggle("##accentAuto", &t.accentAuto, ImGui::GetFrameHeight() * 0.95f)) {
        changed = true;
    }
    if (!t.accentAuto) {
        RowLabel("Color de acento");
        changed |= ImGui::ColorEdit3("##accent", t.accent, ImGuiColorEditFlags_NoInputs);
    }
    EndGroupCard();

    SectionHeader(icons::kVisual, "Vidrio");
    BeginGroupCard("glass");
    RowLabel("Desenfoque");
    changed |= ImGui::SliderFloat("##blur", &t.blur, 0.0f, 1.0f, "%.2f");
    RowLabel("Opacidad del vidrio");
    changed |= ImGui::SliderFloat("##opacity", &t.glassOpacity, 0.1f, 0.95f, "%.2f");
    RowLabel("Oscurecer el juego");
    changed |= ImGui::SliderFloat("##dim", &t.backgroundDim, 0.0f, 0.8f, "%.2f");
    RowLabel("Esquinas");
    changed |= ImGui::SliderFloat("##rounding", &t.rounding, 0.0f, 28.0f, "%.0f px");
    RowLabel("Tamaño de letra");
    ImGui::SliderFloat("##font", &t.fontSize, 13.0f, 28.0f, "%.0f px");
    if (ImGui::IsItemDeactivatedAfterEdit()) {  // se regenera al soltar (cuesta unos ms)
        theme::RequestFontRebuild();
        changed = true;
    }
    RowLabel("Animaciones");
    changed |= Toggle("##anim", &t.animations, ImGui::GetFrameHeight() * 0.95f);
    EndGroupCard();

    SectionHeader(icons::kHud, "HUD en pantalla");
    BeginGroupCard("hud");
    {
        render::hud::Style& h = render::hud::GetStyle();
        bool hudChanged = false;
        RowLabel("Letra");
        static const char* const kFonts[] = {"Pixel", "Luma"};
        int font = static_cast<int>(h.font);
        if (Segmented("hudfont", kFonts, 2, &font)) {
            h.font = static_cast<render::hud::FontChoice>(font);
            hudChanged = true;
        }
        if (h.font == render::hud::FontChoice::Pixel) {
            ImGui::SameLine();
            ImGui::TextDisabled(render::hud::PixelFontAvailable() ? "(la del juego)" : "(no se encontró: se usa Luma)");
        }
        RowLabel("Fondo de las cajas");
        hudChanged |= ImGui::SliderFloat("##hudbg", &h.background, 0.0f, 1.0f, "%.2f");
        RowLabel("Sombra del texto");
        hudChanged |= Toggle("##hudshadow", &h.textShadow, ImGui::GetFrameHeight() * 0.95f);
        RowLabel("Esquinas");
        hudChanged |= ImGui::SliderFloat("##hudround", &h.rounding, 0.0f, 6.0f, "%.0f");
        RowLabel("Borde");
        hudChanged |= Toggle("##hudborder", &h.border, ImGui::GetFrameHeight() * 0.95f);
        RowLabel("Color del texto");
        hudChanged |= ImGui::ColorEdit3("##hudtext", h.text, ImGuiColorEditFlags_NoInputs);
        RowLabel("Tecla apretada", "(keystrokes)");
        hudChanged |= ImGui::ColorEdit3("##hudpressed", h.pressed, ImGuiColorEditFlags_NoInputs);
        RowLabel("Posiciones");
        if (GlassButton("Abrir el editor de HUD", icons::kMove, ImVec2(0, 0))) {
            hudeditor::Begin();
        }
        if (GlassButton("Estilo de fábrica", nullptr, ImVec2(0, 0))) {
            h = render::hud::Style{};
            hudChanged = true;
        }
        if (hudChanged) {
            config::MarkDirty();
        }
    }
    EndGroupCard();

    SectionHeader(icons::kSparkle, "Sonido");
    BeginGroupCard("sound");
    RowLabel("Efectos de sonido");
    if (Toggle("##sounds", &t.sounds, ImGui::GetFrameHeight() * 0.95f)) {
        changed = true;
    }
    ImGui::BeginDisabled(!t.sounds);
    RowLabel("Volumen");
    ImGui::SliderFloat("##volume", &t.soundVolume, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sound::SetVolume(t.soundVolume);
        sound::Play(sound::Effect::ToggleOn);
        changed = true;
    }
    RowLabel("Probar");
    if (GlassButton("Abrir", nullptr, ImVec2(0, 0))) sound::Play(sound::Effect::Open);
    ImGui::SameLine();
    if (GlassButton("Encender", nullptr, ImVec2(0, 0))) sound::Play(sound::Effect::ToggleOn);
    ImGui::SameLine();
    if (GlassButton("Apagar", nullptr, ImVec2(0, 0))) sound::Play(sound::Effect::ToggleOff);
    ImGui::EndDisabled();
    EndGroupCard();

    ImGui::Dummy(ImVec2(0, Fs() * 0.4f));
    if (GlassButton("Restablecer tema", nullptr, ImVec2(0, 0))) {
        const float oldFont = t.fontSize;
        t = theme::Settings{};
        if (t.fontSize != oldFont) {
            theme::RequestFontRebuild();
        }
        sound::SetVolume(t.soundVolume);
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
    PageHeader("Perfiles", "Un perfil guarda TODA la configuración: módulos, opciones, teclas y tema.");
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    const std::string active = config::ActiveProfile();
    std::string toLoad;
    std::string toDelete;

    SectionHeader(icons::kProfiles, "Tus perfiles", static_cast<int>(g_profiles.size()));
    for (const std::string& name : g_profiles) {
        ImGui::PushID(name.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = fs * 3.0f;
        const bool isActive = name == active;
        ImGui::InvisibleButton("##row", ImVec2(width, height));
        const float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + width, pos.y + height);
        glass::Surface(draw, pos, max, Rounding() * 0.7f, ImGui::GetStyle().Alpha, hover);
        if (isActive) {
            draw->AddRect(pos, max, Col(theme::Accent(0.6f)), Rounding() * 0.7f, 0, 1.5f);
        }
        const float circle = fs * 1.9f;
        const ImVec2 c(pos.x + fs * 0.8f + circle * 0.5f, pos.y + height * 0.5f);
        draw->AddCircleFilled(c, circle * 0.5f, Col(isActive ? theme::Accent(1.0f) : p.field), 32);
        icons::Draw(draw, icons::kProfiles, c, fs * 1.1f, Col(isActive ? p.onAccentText : p.text));
        ImFont* medium = theme::GetFonts().medium;
        draw->AddText(medium, medium->FontSize, ImVec2(c.x + circle * 0.5f + fs * 0.7f, pos.y + (height - medium->FontSize) * 0.5f),
                      Col(p.text), name.c_str());
        if (isActive) {
            const ImVec2 ns = medium->CalcTextSizeA(medium->FontSize, FLT_MAX, 0.0f, name.c_str());
            draw->AddText(ImVec2(c.x + circle * 0.5f + fs * 1.3f + ns.x, pos.y + (height - fs) * 0.5f),
                          Col(p.textDim), "activo");
        } else {
            ImGui::SetCursorScreenPos(ImVec2(max.x - fs * 11.0f, pos.y + (height - ImGui::GetFrameHeight() * 1.1f) * 0.5f));
            if (GlassButton("Cargar", nullptr, ImVec2(fs * 5.0f, 0))) toLoad = name;
            ImGui::SameLine(0.0f, fs * 0.4f);
            if (GlassButton("Borrar", icons::kClose, ImVec2(fs * 5.2f, 0))) toDelete = name;
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x, max.y + fs * 0.5f));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    if (!toLoad.empty() && config::SwitchProfile(toLoad)) {
        theme::Apply();
        g_profilesDirty = true;
    }
    if (!toDelete.empty() && config::DeleteProfile(toDelete)) {
        g_profilesDirty = true;
    }

    SectionHeader(icons::kSparkle, "Nuevo perfil");
    BeginGroupCard("new");
    ImGui::SetNextItemWidth(fs * 14.0f);
    ImGui::InputTextWithHint("##newprofile", "Nombre", g_newProfile, sizeof(g_newProfile));
    ImGui::SameLine();
    const bool valid = config::IsValidProfileName(g_newProfile);
    ImGui::BeginDisabled(!valid);
    if (GlassButton("Crear (copia del actual)", nullptr, ImVec2(0, ImGui::GetFrameHeight()), true) &&
        config::CreateProfile(g_newProfile)) {
        g_newProfile[0] = '\0';
        g_profilesDirty = true;
    }
    ImGui::EndDisabled();
    if (!valid && g_newProfile[0] != '\0') {
        ImGui::TextDisabled("Solo letras, números, espacios, '-' y '_' (máx. 32).");
    }
    EndGroupCard();

    SectionHeader(icons::kClose, "Peligro");
    BeginGroupCard("danger");
    if (GlassButton("Restablecer este perfil...", nullptr, ImVec2(0, 0))) {
        ImGui::OpenPopup("##confirm_reset");
    }
    if (ImGui::BeginPopup("##confirm_reset")) {
        ImGui::Text("¿Volver todo el perfil \"%s\" a los valores de fábrica?", active.c_str());
        if (GlassButton("Sí, restablecer", nullptr, ImVec2(0, 0), true)) {
            config::ResetActiveProfile();
            theme::Apply();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (GlassButton("Cancelar", nullptr, ImVec2(0, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    EndGroupCard();
}

void DrawSettingsPage() {
    PageHeader("Ajustes", "Teclas de Luma y archivos.");
    SectionHeader(icons::kKey, "Teclas de Luma");
    BeginGroupCard("keys");
    RowLabel("Abrir / cerrar el menú");
    KeybindButton("##menukey", config::g_menuKey.load(), g_capture == CaptureTarget::MenuKey, CaptureTarget::MenuKey, nullptr);
    RowLabel("Descargar Luma");
    KeybindButton("##unloadkey", config::g_unloadKey.load(), g_capture == CaptureTarget::UnloadKey,
                  CaptureTarget::UnloadKey, nullptr);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Esc también cierra el menú. Limitación conocida: si la tecla del menú es una "
                        "letra, escribirla en el chat abre el menú (se corrige en la Fase 7).");
    ImGui::PopTextWrapPos();
    EndGroupCard();

    SectionHeader(icons::kAll, "Archivos");
    BeginGroupCard("files");
    if (GlassButton("Abrir la carpeta de Luma", nullptr, ImVec2(0, 0))) {
        ShellExecuteW(nullptr, L"open", paths::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Config, perfiles y logs.");
    EndGroupCard();
}

// =============================================================================
//  Página de Benchmark (Fase 4)
// =============================================================================

// Los nombres de las optimizaciones activas ("Timer de 1 ms + Prioridad...").
std::string ActiveOptimizations() {
    std::string out;
    for (const auto& m : Manager::All()) {
        if (m->IsEnabled() && m->Id().rfind("opt_", 0) == 0) {
            if (!out.empty()) {
                out += " + ";
            }
            out += m->Name();
        }
    }
    return out;
}

const char* StatusText(perf::windows::Status s) {
    switch (s) {
        case perf::windows::Status::On:  return "Activado";
        case perf::windows::Status::Off: return "Desactivado";
        default:                          return "No disponible";
    }
}

// Una fila de consejo: título, estado (con color), explicación y un botón
// que abre la página de Configuración de Windows donde se cambia.
void TipRow(const char* title, const char* status, bool good, const char* text, const wchar_t* settingsUri) {
    const theme::Palette& p = theme::P();
    ImGui::PushID(title);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(theme::GetFonts().medium);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::SameLine();
    const ImVec4 color = good ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.75f, 0.30f, 1.0f);
    ImGui::TextColored(theme::WithAlpha(color, ImGui::GetStyle().Alpha), "%s", status);
    if (settingsUri != nullptr) {
        ImGui::SameLine();
        if (GlassButton("Abrir en Windows", nullptr, ImVec2(0, 0))) {
            ShellExecuteW(nullptr, L"open", settingsUri, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, Fs() * 0.3f));
    ImGui::PopID();
}

void DrawBenchmarkPage() {
    PageHeader("Benchmark", "Mide FPS de forma comparable y prueba si las optimizaciones sirven.");
    const float fs = Fs();
    const perf::benchmark::State state = perf::benchmark::GetState();
    const bool idle = state == perf::benchmark::State::Idle;

    SectionHeader(icons::kChart, "Nueva prueba");
    BeginGroupCard("bench");
    ImGui::BeginDisabled(!idle);
    RowLabel("Nombre");
    ImGui::InputTextWithHint("##benchlabel", "Por ejemplo: sin optimizaciones", g_benchLabel, sizeof(g_benchLabel));
    RowLabel("Duración");
    ImGui::SliderInt("##benchsecs", &g_benchSeconds, 10, 300, "%d s");
    RowLabel("Modo");
    static const char* const kModes[] = {"Con mi HUD", "Luma en reposo"};
    Segmented("benchmode", kModes, 2, &g_benchMode);
    RowLabel("Optimizaciones");
    const std::string optimizations = ActiveOptimizations();
    ImGui::TextUnformatted(optimizations.empty() ? "ninguna" : optimizations.c_str());
    ImGui::EndDisabled();

    if (idle) {
        if (GlassButton("Empezar (el menú se cierra)", icons::kChart, ImVec2(0, 0), true)) {
            const auto mode = g_benchMode == 1 ? perf::benchmark::Mode::Resting : perf::benchmark::Mode::WithHud;
            if (perf::benchmark::Start(g_benchSeconds, 5.0, g_benchLabel, mode, optimizations)) {
                notify::Push(notify::Kind::Info, "Benchmark en 5 s: no abras el menú y no te muevas");
                // Cerrar el menú desde el hilo de la ventana (ver input.h).
                render::input::RequestMenuToggle();
            }
        }
    } else {
        const char* what = state == perf::benchmark::State::Countdown ? "Empieza en" : "Grabando: faltan";
        ImGui::Text("%s %.0f s", what, perf::benchmark::SecondsLeft());
        ImGui::SameLine();
        if (GlassButton("Cancelar", icons::kClose, ImVec2(0, 0))) {
            perf::benchmark::Cancel("cancelado desde el menú");
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Para comparar: quédate quieto en el mismo lugar y mirando lo mismo en cada prueba. "
                        "\"Luma en reposo\" no dibuja nada durante la prueba: su costo es ~0,001 ms, así que es "
                        "la referencia \"sin Luma\". Después activa UNA optimización y repite.");
    ImGui::PopTextWrapPos();
    EndGroupCard();

    const std::vector<perf::benchmark::Result> results = perf::benchmark::Results();
    SectionHeader(icons::kDiagnostics, "Resultados de esta sesión", static_cast<int>(results.size()));
    if (results.empty()) {
        ImGui::TextDisabled("Todavía no hiciste ninguna prueba.");
    } else {
        constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                           ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("##results", 7, kTable)) {
            ImGui::TableSetupColumn("Nombre", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Modo", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Optimizaciones", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("1% low", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Frametime", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Luma", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (auto it = results.rbegin(); it != results.rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->label.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->mode.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->optimizations.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", it->averageFps);
                ImGui::TableNextColumn(); ImGui::Text("%.1f", it->low1Fps);
                ImGui::TableNextColumn(); ImGui::Text("%.2f ms", it->averageMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f ms", it->lumaCostMs);
            }
            ImGui::EndTable();
        }
    }
    if (GlassButton("Abrir la carpeta de benchmarks", nullptr, ImVec2(0, 0))) {
        std::error_code ec;
        const std::filesystem::path dir = paths::DataDir() / L"benchmarks";
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("resumen.csv = una fila por prueba; un .csv por prueba con cada frame.");

    // --- Consejos de Windows ---------------------------------------------------
    if (g_windowsInfoDirty) {
        g_windowsInfo = perf::windows::Read();
        g_windowsInfoDirty = false;
    }
    const perf::windows::Info& w = g_windowsInfo;
    using perf::windows::Status;
    SectionHeader(icons::kSettings, "Consejos de Windows");
    BeginGroupCard("windows");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Luma solo te muestra cómo están estas opciones: nunca las cambia por su cuenta. "
                           "Prueba cada una con el benchmark.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, fs * 0.3f));
    TipRow("Modo de juego", StatusText(w.gameMode), w.gameMode == Status::On,
           "Windows le da prioridad al juego y evita instalar actualizaciones mientras juegas. Recomendado: "
           "activado. (Configuración > Juegos > Modo de juego.)",
           L"ms-settings:gaming-gamemode");
    TipRow("Programación de GPU acelerada por hardware", StatusText(w.gpuScheduling), w.gpuScheduling == Status::On,
           "La tarjeta gráfica organiza su propio trabajo: puede bajar la latencia. Con GPUs y drivers recientes, "
           "suele convenir activado (pide reiniciar). (Configuración > Pantalla > Gráficos > Cambiar la "
           "configuración predeterminada de gráficos.)",
           L"ms-settings:display-advancedgraphics");
    TipRow("Optimizaciones para juegos en ventana", StatusText(w.windowedOptimizations),
           w.windowedOptimizations == Status::On,
           "Minecraft Bedrock juega en ventana: esta opción le da casi la misma latencia que la pantalla completa. "
           "(Mismo lugar que la anterior.)",
           L"ms-settings:display-advancedgraphics");
    const std::string plan = "Plan actual: " + (w.powerPlan.empty() ? std::string("desconocido") : w.powerPlan);
    TipRow("Plan de energía", plan.c_str(), w.powerPlanIsHighPerformance,
           "En portátiles, \"Equilibrado\" puede bajar la frecuencia de la CPU. \"Alto rendimiento\" (o el modo de "
           "energía \"Mejor rendimiento\" en Windows 11) la mantiene alta. Enchufa el cargador para jugar.",
           L"ms-settings:powersleep");
    const std::string gpu = "Ahora: " + w.gpuPreference;
    TipRow("GPU para Minecraft", gpu.c_str(), w.gpuPreference == "Alto rendimiento",
           "En portátiles con dos GPU (integrada + dedicada), elige \"Alto rendimiento\" para Minecraft en "
           "Configuración > Pantalla > Gráficos. Si tu PC tiene una sola GPU, no hace falta.",
           L"ms-settings:display-advancedgraphics");
    if (GlassButton("Volver a leer", nullptr, ImVec2(0, 0))) {
        g_windowsInfoDirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Nada de \"limpiadores de RAM\" ni trucos de registro: no tienen evidencia.");
    EndGroupCard();
}

struct HistoryStats {
    float averageMs = 0.0f;
    float maxMs = 0.0f;
    float percentile99Ms = 0.0f;
    float openCostAvgUs = 0.0f;
    float openCostMaxUs = 0.0f;
};

HistoryStats ComputeStats(const perf::History& history) {
    HistoryStats stats;
    if (history.count == 0) {
        return stats;
    }
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
    const std::size_t index = (history.count * 99) / 100;
    auto end = sorted.begin() + static_cast<std::ptrdiff_t>(history.count);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), end);
    stats.percentile99Ms = sorted[index];
    return stats;
}

// Una "cifra grande" con su etiqueta, para el panel de diagnóstico.
void StatTile(const char* label, const char* value, float width) {
    const theme::Palette& p = theme::P();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = Fs() * 4.2f;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    glass::Surface(draw, pos, ImVec2(pos.x + width, pos.y + height), Rounding() * 0.7f, ImGui::GetStyle().Alpha, 0.0f);
    ImFont* title = theme::GetFonts().title;
    draw->AddText(title, title->FontSize, ImVec2(pos.x + Fs() * 0.9f, pos.y + Fs() * 0.6f), Col(p.text), value);
    draw->AddText(ImVec2(pos.x + Fs() * 0.9f, pos.y + height - Fs() * 1.6f), Col(p.textDim), label);
}

void DrawDiagnosticsPage(const RenderInfo& info) {
    PageHeader("Diagnóstico", "Rendimiento del juego y costo de Luma.");
    const perf::History& history = perf::GetHistory();
    const HistoryStats stats = ComputeStats(history);
    const float fps = stats.averageMs > 0.0f ? 1000.0f / stats.averageMs : 0.0f;
    const float low1 = stats.percentile99Ms > 0.0f ? 1000.0f / stats.percentile99Ms : 0.0f;
    const perf::Summary summary = perf::LastSummary();

    SectionHeader(icons::kDiagnostics, "Ahora");
    const float gap = Fs() * 0.8f;
    const float tile = (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0f", fps);
    StatTile("FPS", buffer, tile);
    ImGui::SameLine(0, gap);
    std::snprintf(buffer, sizeof(buffer), "%.0f", low1);
    StatTile("1% low", buffer, tile);
    ImGui::SameLine(0, gap);
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", stats.averageMs);
    StatTile("Frametime", buffer, tile);
    ImGui::SameLine(0, gap);
    if (summary.valid && summary.closedFrames > 0) {
        std::snprintf(buffer, sizeof(buffer), "%.3f ms", summary.averageCostUs / 1000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "...");
    }
    StatTile("Costo de Luma (menú cerrado)", buffer, tile);

    SectionHeader(icons::kChart, "Frametime");
    BeginGroupCard("graph");
    const float scaleMax = std::max(33.3f, stats.maxMs * 1.1f);
    const int offset = history.count == perf::kHistorySize ? static_cast<int>(history.next) : 0;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PlotLines("##frametime", history.frameMs.data(), static_cast<int>(history.count), offset, nullptr, 0.0f,
                     scaleMax, ImVec2(ImGui::GetContentRegionAvail().x - Fs() * 1.8f, Fs() * 6.0f));
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Últimos %zu frames · máx %.2f ms · más abajo = mejor, picos = tirones", history.count, stats.maxMs);
    EndGroupCard();

    SectionHeader(icons::kPerformance, "Costo de Luma");
    BeginGroupCard("cost");
    if (summary.valid && summary.closedFrames > 0) {
        ImGui::Text("Menú cerrado (últimos %.0f s): %.4f ms promedio, %.4f ms máximo", summary.seconds,
                    summary.averageCostUs / 1000.0, summary.maxCostUs / 1000.0);
    } else {
        ImGui::TextDisabled("Menú cerrado: deja el menú cerrado 30 s y vuelve a abrirlo.");
    }
    ImGui::Text("Menú abierto (ahora, con el vidrio): %.3f ms promedio, %.3f ms máximo",
                stats.openCostAvgUs / 1000.0f, stats.openCostMaxUs / 1000.0f);
    ImGui::TextDisabled("DirectX 12 · %u x %u · %s · %u buffers", info.width, info.height, info.formatName, info.bufferCount);
    EndGroupCard();
}

// =============================================================================
//  Barra lateral
// =============================================================================

// Una entrada de la barra lateral: ícono + texto. La "píldora" que marca la
// elegida se dibuja aparte y se desliza de una a otra (ver Sidebar).
bool NavItem(const char* label, const char* icon, bool selected) {
    const theme::Palette& p = theme::P();
    const float height = Fs() * 2.35f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered && !selected ? 1.0f : 0.0f, 18.0f);
    const float active = Animate(ImGui::GetItemID() + 1, selected ? 1.0f : 0.0f, 14.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hover > 0.01f) {
        draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), Col(p.surface, hover), height * 0.5f);
    }
    if (selected) {
        g_navPillTarget = pos.y - ImGui::GetWindowPos().y;
        g_navPillHeight = height;
    }
    const ImVec4 color = Mix(theme::WithAlpha(p.text, 0.72f), p.onAccentText, active);
    const float iconSize = Fs() * 1.15f;
    const float nudge = hover * Fs() * 0.2f;
    icons::Draw(draw, icon, ImVec2(pos.x + Fs() * 1.25f + nudge, pos.y + height * 0.5f), iconSize, Col(color));
    draw->AddText(ImVec2(pos.x + Fs() * 2.6f + nudge, pos.y + (height - Fs()) * 0.5f), Col(color), label);
    return clicked;
}

void NavLabel(const char* text) {
    ImGui::Dummy(ImVec2(0, Fs() * 0.5f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Fs() * 0.6f);
    ImGui::TextDisabled("%s", text);
}

void Sidebar() {
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Marca: el logo de Luma y su nombre.
    const ImVec2 brand = ImGui::GetCursorScreenPos();
    const float logo = fs * 2.3f;
    assets::DrawLogo(draw, ImVec2(brand.x + fs * 0.6f + logo * 0.5f, brand.y + logo * 0.5f), logo, Col(p.text));
    ImFont* title = theme::GetFonts().title;
    draw->AddText(title, title->FontSize, ImVec2(brand.x + fs * 1.1f + logo, brand.y + (logo - title->FontSize) * 0.5f),
                  Col(p.text), kName);
    ImGui::Dummy(ImVec2(0, logo + fs * 0.6f));

    // Todo lo de la barra se dibuja en dos "canales": la píldora en el de
    // abajo y los botones encima, así el texto queda sobre la píldora.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    g_navPillTarget = -1.0f;

    NavLabel("MÓDULOS");
    auto category = [](const char* label, const char* icon, Category c) {
        const bool selected = g_page == Page::Modules && g_categoryFilter == c;
        if (NavItem(label, icon, selected) && !(selected && g_detail == nullptr)) {
            g_page = Page::Modules;
            g_categoryFilter = c;
            g_detail = nullptr;
            OnNavigate();
        }
    };
    category("Todos", icons::kAll, Category::Count);
    for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
        const auto c = static_cast<Category>(i);
        category(modules::CategoryName(c), CategoryIcon(c), c);
    }

    NavLabel("LUMA");
    auto page = [](const char* label, const char* icon, Page pg) {
        if (NavItem(label, icon, g_page == pg) && g_page != pg) {
            g_page = pg;
            g_detail = nullptr;
            g_profilesDirty = true;
            g_windowsInfoDirty = true;
            OnNavigate();
        }
    };
    page("Tema", icons::kTheme, Page::Theme);
    page("Perfiles", icons::kProfiles, Page::Profiles);
    page("Ajustes", icons::kSettings, Page::Settings);
    page("Benchmark", icons::kChart, Page::Benchmark);
    page("Diagnóstico", icons::kDiagnostics, Page::Diagnostics);

    // La píldora: se desliza hasta la entrada elegida con un leve rebote.
    draw->ChannelsSetCurrent(0);
    if (g_navPillTarget >= 0.0f) {
        if (g_navPillY < 0.0f || !theme::Get().animations) {
            g_navPillY = g_navPillTarget;
        } else {
            g_navPillY = ImLerp(g_navPillY, g_navPillTarget, std::min(1.0f, ImGui::GetIO().DeltaTime * 15.0f));
        }
        const ImVec2 win = ImGui::GetWindowPos();
        const float left = win.x + ImGui::GetStyle().WindowPadding.x;
        const float right = win.x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const ImVec2 min(left, win.y + g_navPillY);
        const ImVec2 max(right, min.y + g_navPillHeight);
        draw->AddRectFilled(ImVec2(min.x, min.y + 2), ImVec2(max.x, max.y + 3), Col(p.shadow, 0.6f), g_navPillHeight * 0.5f);
        draw->AddRectFilled(min, max, Col(theme::Accent(1.0f)), g_navPillHeight * 0.5f);
    }
    draw->ChannelsMerge();

    // Al pie: perfil y versión.
    const float footerY = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - fs * 2.6f;
    if (ImGui::GetCursorScreenPos().y < footerY) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + fs * 0.6f, footerY));
        ImGui::TextDisabled("%s", ("Perfil: " + config::ActiveProfile()).c_str());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fs * 0.6f);
        ImGui::TextDisabled("Luma %s", kVersion);
    }
}

}  // namespace

// =============================================================================
//  Funciones públicas
// =============================================================================

void OnOpened() {
    g_openedAt = perf::Now();
    g_pageChangedAt = g_openedAt;
    g_closing.store(false);
    g_lastHovered = 0;
    sound::Play(sound::Effect::Open);
}

void OnClosed() {
    if (hudeditor::IsActive()) {
        hudeditor::End();
    }
    g_closedAt = perf::Now();
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
        return;
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
    sound::Play(sound::Effect::ToggleOn);
    config::MarkDirty();
}

void Draw(const RenderInfo& info) {
    const bool closing = g_closing.load(std::memory_order_relaxed);
    // Editor de HUD: reemplaza al menú mientras está abierto.
    if (hudeditor::IsActive() && !closing) {
        hudeditor::Draw();
        if (!hudeditor::IsActive()) {
            OnOpened();  // al salir del editor, el menú vuelve a aparecer con su animación
        }
        return;
    }

    // --- Animación de apertura o de cierre ---------------------------------------
    float appear = 1.0f;
    float scale = 1.0f;
    if (closing) {
        const float t = static_cast<float>(MsSince(g_closedAt) / kCloseMs);
        if (t >= 1.0f) {
            g_closing.store(false);
            return;
        }
        appear = 1.0f - t * t;
        scale = 0.97f + 0.03f * appear;
    } else if (theme::Get().animations) {
        const float t = static_cast<float>(MsSince(g_openedAt) / kOpenMs);
        appear = EaseOut(t);
        scale = 0.94f + 0.06f * EaseOutBack(t);  // crece hasta su tamaño con un leve rebote
    }
    g_appear = appear;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fs = Fs();

    // Oscurecer un poco el juego detrás (el vidrio ya lo desenfoca).
    const float dim = theme::Get().backgroundDim * appear;
    if (dim > 0.001f) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.02f, 0.02f, 0.03f, dim)));
    }

    // Ventana anfitriona invisible que cubre la pantalla; los paneles de
    // vidrio se dibujan dentro, centrados y de tamaño fijo.
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##luma_menu", nullptr, kFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float width = std::clamp(viewport->Size.x * 0.72f, fs * 58.0f, fs * 100.0f) * scale;
        const float height = std::clamp(viewport->Size.y * 0.76f, fs * 34.0f, fs * 62.0f) * scale;
        const float rise = (1.0f - appear) * fs * 1.6f;
        const ImVec2 origin(viewport->Pos.x + (viewport->Size.x - width) * 0.5f,
                            viewport->Pos.y + (viewport->Size.y - height) * 0.5f + rise);
        const float sidebarWidth = fs * 15.5f * scale;
        const float gap = fs * 0.9f;
        const float r = Rounding() * 1.25f;

        const ImVec2 sideMin = origin;
        const ImVec2 sideMax(origin.x + sidebarWidth, origin.y + height);
        const ImVec2 mainMin(sideMax.x + gap, origin.y);
        const ImVec2 mainMax(origin.x + width, origin.y + height);
        glass::Panel(draw, sideMin, sideMax, r, appear);
        glass::Panel(draw, mainMin, mainMax, r, appear);

        const float pad = fs * 1.2f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));

        ImGui::SetCursorScreenPos(sideMin);
        ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        Sidebar();
        ImGui::EndChild();

        // Transición entre páginas: el contenido nuevo aparece y se desliza.
        float pageT = 1.0f;
        if (theme::Get().animations && !closing) {
            pageT = EaseOut(static_cast<float>(MsSince(g_pageChangedAt) / kPageMs));
        }
        const float slide = (1.0f - pageT) * fs * 2.0f;
        ImGui::SetCursorScreenPos(ImVec2(mainMin.x + slide, mainMin.y));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear * pageT);
        ImGui::BeginChild("##main", ImVec2(mainMax.x - mainMin.x, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground);
        if (g_page == Page::Modules && g_detail != nullptr) {
            DrawModuleDetail(*g_detail);
        } else {
            switch (g_page) {
                case Page::Theme:       DrawThemePage(); break;
                case Page::Profiles:    DrawProfilesPage(); break;
                case Page::Settings:    DrawSettingsPage(); break;
                case Page::Benchmark:   DrawBenchmarkPage(); break;
                case Page::Diagnostics: DrawDiagnosticsPage(info); break;
                case Page::Modules:     DrawModulesPage(); break;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace luma::render::menu
