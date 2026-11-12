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
#include <cstdint>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>  // ImLerp

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/lyrics.h"
#include "core/media.h"
#include "core/keys.h"
#include "core/paths.h"
#include "core/version.h"
#include "i18n.h"
#include "skin_pack.h"
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
#include "render/music.h"
#include "render/module_options.h"
#include "render/input.h"
#include "render/notifications.h"
#include "render/skin_heads.h"
#include "render/sound.h"
#include "render/theme.h"
#include "render/ui.h"
#include "sdk/network.h"
#include "sdk/player.h"

namespace luma::render::menu {

namespace {

using modules::Category;
using modules::Manager;
using modules::Module;

// =============================================================================
//  Estado
// =============================================================================

// The sidebar has 4 entries: Modules, Music, Closet and Settings. Theme,
// Profiles, Benchmark and Diagnostics are tabs inside Settings (they keep
// their own Page value so old links still work: they open the right tab).
enum class Page { Modules, Theme, Profiles, Settings, Benchmark, Diagnostics, Closet, Music };
int g_settingsTab = 0;  // 0 general, 1 appearance, 2 profiles, 3 performance
int g_musicTab = 0;     // 0 lyrics, 1 styles, 2 halo
constexpr const char* kMusicIcon = icons::kMusic;

Page g_page = Page::Modules;
Category g_categoryFilter = Category::Count;  // Count = todas
Module* g_detail = nullptr;                   // módulo con la página de opciones abierta
char g_search[64] = "";

// Animaciones.
std::int64_t g_openedAt = 0;
std::int64_t g_closedAt = 0;
bool g_radialWhenClosed = true;  // where the menu was when it closed (quick reopen)
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
enum class CaptureTarget { None, Module, MenuKey, UnloadKey, Value };
CaptureTarget g_capture = CaptureTarget::None;
Module* g_captureModule = nullptr;
int* g_captureValue = nullptr;  // CaptureTarget::Value: a module's own key (ui::Key)

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

// Are widget animations on? On the Music page the MUSIC efficiency mode
// decides; everywhere else, the general one. (They're separate on purpose:
// the general efficiency mode never touches Luma Music.)
bool g_musicUi = false;
bool AnimOn() {
    return g_musicUi ? music::UiAnimations() : theme::AnimationsOn();
}

// Valor animado hacia "target", guardado por ID en el almacén de ImGui.
float Animate(ImGuiID id, float target, float speed = 14.0f) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float value = storage->GetFloat(id, target);
    if (AnimOn()) {
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

// --- Reactive buttons --------------------------------------------------------------
// Every button reacts to the touch like a real one:
//   - pressed, it sinks a little (it shrinks to ~96%);
//   - released, it springs back with a tiny bounce (a spring that overshoots
//     a hair and settles);
//   - a click leaves a soft ripple of light that spreads from the mouse and
//     fades in half a second.
// Cheap: the button is drawn as always and then its corners are moved
// (scaled around its center); the ripple is one circle for half a second.
// With "animations" off in the theme, nothing of this happens.
struct Press {
    ImGuiID id = 0;
    int firstVertex = 0;
    ImVec2 min, max;
    float scale = 1.0f;
};

// Call right after the button's InvisibleButton, BEFORE drawing it.
Press BeginPress(ImVec2 min, ImVec2 max, float depth = 0.04f) {
    Press pr;
    pr.id = ImGui::GetItemID();
    pr.min = min;
    pr.max = max;
    pr.firstVertex = ImGui::GetWindowDrawList()->VtxBuffer.Size;
    if (!AnimOn()) return pr;
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID kScale = pr.id ^ 0x51A7E001u, kSpeed = pr.id ^ 0x51A7E002u;
    const ImGuiID kTime = pr.id ^ 0x51A7E003u, kX = pr.id ^ 0x51A7E004u, kY = pr.id ^ 0x51A7E005u;
    const bool held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    float x = st->GetFloat(kScale, 1.0f);
    float v = st->GetFloat(kSpeed, 0.0f);
    const float target = held ? 1.0f - depth : 1.0f;
    // A slightly bouncy spring (damping 0.45 of "critical": it overshoots a
    // little on release). Two small steps per frame keep it stable.
    constexpr float kStiffness = 420.0f;
    const float damping = 2.0f * 0.45f * std::sqrt(kStiffness);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f) * 0.5f;
    for (int i = 0; i < 2; ++i) {
        v += (kStiffness * (target - x) - damping * v) * dt;
        x += v * dt;
    }
    if (std::fabs(x - target) < 0.0004f && std::fabs(v) < 0.002f) {
        x = target;
        v = 0.0f;
    }
    st->SetFloat(kScale, x);
    st->SetFloat(kSpeed, v);
    pr.scale = x;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        st->SetFloat(kTime, static_cast<float>(ImGui::GetTime()));
        st->SetFloat(kX, ImGui::GetIO().MousePos.x - min.x);
        st->SetFloat(kY, ImGui::GetIO().MousePos.y - min.y);
    }
    return pr;
}

// Call AFTER drawing the button: draws the ripple and applies the press.
// "round" = a round button (the ripple grows from its center, inside it).
void EndPress(const Press& pr, ImVec4 rippleColor, bool ripple = true, bool round = false) {
    if (!AnimOn()) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGuiStorage* st = ImGui::GetStateStorage();
    const float started = st->GetFloat(pr.id ^ 0x51A7E003u, -100.0f);
    const float age = static_cast<float>(ImGui::GetTime()) - started;
    constexpr float kRipple = 0.55f;
    if (ripple && age >= 0.0f && age < kRipple) {
        const float t = age / kRipple;
        const float grow = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // fast, then slow
        const float fade = (1.0f - t) * (1.0f - t);
        const ImVec2 size(pr.max.x - pr.min.x, pr.max.y - pr.min.y);
        if (round) {
            const ImVec2 c(pr.min.x + size.x * 0.5f, pr.min.y + size.y * 0.5f);
            draw->AddCircleFilled(c, size.x * 0.5f * grow, Col(theme::WithAlpha(rippleColor, rippleColor.w * fade)), 40);
        } else {
            const ImVec2 at(pr.min.x + st->GetFloat(pr.id ^ 0x51A7E004u, size.x * 0.5f),
                            pr.min.y + st->GetFloat(pr.id ^ 0x51A7E005u, size.y * 0.5f));
            // Far enough to reach the farthest corner.
            const float dx = std::max(at.x - pr.min.x, pr.max.x - at.x), dy = std::max(at.y - pr.min.y, pr.max.y - at.y);
            draw->PushClipRect(pr.min, pr.max, true);
            draw->AddCircleFilled(at, std::sqrt(dx * dx + dy * dy) * grow, Col(theme::WithAlpha(rippleColor, rippleColor.w * fade)), 48);
            draw->PopClipRect();
        }
    }
    if (std::fabs(pr.scale - 1.0f) > 0.0003f) {
        const ImVec2 c((pr.min.x + pr.max.x) * 0.5f, (pr.min.y + pr.max.y) * 0.5f);
        for (int i = pr.firstVertex; i < draw->VtxBuffer.Size; ++i) {
            ImVec2& v = draw->VtxBuffer[i].pos;
            v.x = c.x + (v.x - c.x) * pr.scale;
            v.y = c.y + (v.y - c.y) * pr.scale;
        }
    }
}

// An icon that changes (play <-> pause) pops in instead of switching at once.
// Returns 0..1: how far into its entrance the current icon is.
float IconPop(ImGuiID id, const char* icon) {
    if (!AnimOn()) return 1.0f;
    ImGuiStorage* st = ImGui::GetStateStorage();
    const int key = static_cast<int>(reinterpret_cast<std::uintptr_t>(icon) & 0x7FFFFFFF);
    const ImGuiID kIcon = id ^ 0x1C0A0001u, kTime = id ^ 0x1C0A0002u;
    const int last = st->GetInt(kIcon, key);
    if (last != key) st->SetFloat(kTime, static_cast<float>(ImGui::GetTime()));
    st->SetInt(kIcon, key);
    const float age = static_cast<float>(ImGui::GetTime()) - st->GetFloat(kTime, -100.0f);
    return std::clamp(age / 0.22f, 0.0f, 1.0f);
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


// A name that must fit in "maxWidth" (module cards and rows): first it
// shrinks a little; if it still doesn't fit and "maxLines" is 2, it wraps
// into two lines going UP from "bottomY". Nothing is cut in half.
void FitText(ImDrawList* draw, ImFont* font, float x, float bottomY, float maxWidth, ImU32 color, const char* text,
             int maxLines) {
    float size = font->FontSize;
    float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    if (w > maxWidth) {
        size = std::max(font->FontSize * 0.86f, size * maxWidth / w);
        w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    }
    if (w <= maxWidth || maxLines < 2) {
        draw->AddText(font, size, ImVec2(x, bottomY - size), color, text);
        return;
    }
    const char* end = text + std::strlen(text);
    const char* cut = font->CalcWordWrapPositionA(size / font->FontSize, text, end, maxWidth);
    if (cut == text) cut = text + 1;
    const char* second = cut;
    while (*second == ' ') ++second;
    draw->AddText(font, size, ImVec2(x, bottomY - size * 2.02f), color, text, cut);
    draw->AddText(font, size, ImVec2(x, bottomY - size), color, second, end);
}

void CenteredText(ImDrawList* draw, ImFont* font, ImVec2 min, ImVec2 max, ImU32 color, const char* text) {
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
    draw->AddText(font, font->FontSize,
                  ImVec2(min.x + (max.x - min.x - size.x) * 0.5f, min.y + (max.y - min.y - size.y) * 0.5f),
                  color, text);
}

// Scroll suave: la rueda no salta de golpe, sino que el contenido se desliza
// hasta su lugar (frenando al final). Se llama justo después de BeginChild en
// las ventanas con ImGuiWindowFlags_NoScrollWithMouse (así ImGui no hace su
// scroll de golpe). Si mueves la barra con el mouse, se respeta.
void SmoothScroll() {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID targetId = ImGui::GetID("##smooth_target");
    const ImGuiID lastId = ImGui::GetID("##smooth_last");
    const float current = window->Scroll.y;
    const float maxY = window->ScrollMax.y;
    float target = storage->GetFloat(targetId, current);
    const float last = storage->GetFloat(lastId, current);
    if (std::fabs(current - last) > 0.5f) {
        target = current;  // alguien más lo movió (la barra, SetScroll...)
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && maxY > 0.0f) {
        target -= io.MouseWheel * Fs() * 6.0f;
    }
    target = std::clamp(target, 0.0f, std::max(0.0f, maxY));
    float next = target;
    if (AnimOn()) {
        const float k = 1.0f - std::exp(-io.DeltaTime * 13.0f);
        next = current + (target - current) * k;
        if (std::fabs(target - next) < 0.5f) next = target;
    }
    if (next != current) ImGui::SetScrollY(next);
    storage->SetFloat(targetId, target);
    storage->SetFloat(lastId, next);
}

// =============================================================================
//  Widgets
// =============================================================================

// Interruptor de Luma: una ranura fina y una "lámpara" que se desliza. Apagado
// es gris; encendido, la ranura se ilumina con el acento y brilla un poco.
// (El área para hacer clic es la altura completa; el dibujo, un poco más fino.)
bool Toggle(const char* id, bool* value, float height) {
    const float h = std::floor(height * 0.78f);
    const float width = std::floor(h * 1.85f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
    if (clicked) {
        *value = !*value;
        sound::Play(*value ? sound::Effect::ToggleOn : sound::Effect::ToggleOff);
    }
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float t = Animate(ImGui::GetItemID(), *value ? 1.0f : 0.0f, 16.0f);
    const float hover = Animate(ImGui::GetItemID() + 7, hovered ? 1.0f : 0.0f, 18.0f);

    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos(start.x, std::floor(start.y + (height - h) * 0.5f));
    const ImVec2 max(pos.x + width, pos.y + h);
    const Press press = BeginPress(pos, max, 0.08f);
    // El brillo alrededor (solo encendido).
    if (t > 0.01f) {
        const float g = h * 0.22f;
        draw->AddRectFilled(ImVec2(pos.x - g, pos.y - g), ImVec2(max.x + g, max.y + g), Col(theme::Accent(0.16f * t)),
                            (h + g * 2.0f) * 0.5f);
    }
    const ImVec4 off = theme::WithAlpha(p.trackOff, 1.0f + 0.6f * hover);
    draw->AddRectFilled(pos, max, Col(Mix(off, theme::Accent(1.0f), t)), h * 0.5f);
    // La perilla: marfil apagada, oscura (como un hueco con luz alrededor) encendida.
    const float pad = std::max(2.0f, h * 0.16f);
    const float r = h * 0.5f - pad;
    const float cx = pos.x + pad + r + t * (width - (pad + r) * 2.0f);
    const float cy = pos.y + h * 0.5f;
    draw->AddCircleFilled(ImVec2(cx, cy), r, Col(Mix(p.knob, p.onAccentText, t)), 24);
    EndPress(press, ImVec4(0, 0, 0, 0), /*ripple=*/false);
    return clicked;
}

// Botón de Luma con ícono opcional. "primary" = relleno con el acento (uno por
// pantalla, para la acción principal).
bool GlassButton(const char* label, const char* icon, ImVec2 size, bool primary = false) {
    label = Tr(label);
    const theme::Palette& p = theme::P();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float iconSize = Fs() * 1.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);
    const bool hasText = textSize.x > 0.0f;
    if (size.x <= 0.0f) {
        size.x = textSize.x + style.FramePadding.x * 2.0f +
                 (icon ? iconSize + (hasText ? style.ItemInnerSpacing.x : 0.0f) : 0.0f);
    }
    if (size.y <= 0.0f) {
        size.y = ImGui::GetFrameHeight() * 1.05f;
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    HoverFeedback(hovered && !disabled);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 18.0f);
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const Press press = BeginPress(pos, max, disabled ? 0.0f : 0.045f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = std::min(size.y * 0.5f, Rounding() * 0.55f);
    const float alpha = disabled ? 0.45f : 1.0f;
    ImVec4 textColor = p.text;
    if (primary) {
        if (hover > 0.01f) {  // un halo cálido al pasar el mouse
            draw->AddRectFilled(ImVec2(pos.x - 3, pos.y - 3), ImVec2(max.x + 3, max.y + 3),
                                Col(theme::Accent(0.18f * hover), alpha), r + 3);
        }
        draw->AddRectFilled(pos, max, Col(theme::Accent(held ? 0.82f : 0.92f + 0.08f * hover), alpha), r);
        textColor = p.onAccentText;
    } else {
        draw->AddRectFilled(pos, max, Col(theme::WithAlpha(p.field, 1.0f + 0.9f * hover + (held ? 0.6f : 0.0f)), alpha), r);
        draw->AddRect(pos, max, Col(theme::WithAlpha(p.border, 0.55f + 0.45f * hover), alpha), r, 0, 1.0f);
    }
    float x = pos.x + (size.x - textSize.x - (icon ? iconSize + (hasText ? style.ItemInnerSpacing.x : 0.0f) : 0.0f)) * 0.5f;
    if (icon != nullptr) {
        icons::Draw(draw, icon, ImVec2(x + iconSize * 0.5f, pos.y + size.y * 0.5f), iconSize, Col(textColor, alpha));
        x += iconSize + style.ItemInnerSpacing.x;
    }
    if (hasText) {
        draw->AddText(ImVec2(x, pos.y + (size.y - textSize.y) * 0.5f), Col(textColor, alpha), label);
    }
    EndPress(press, theme::WithAlpha(textColor, 0.16f), !disabled);
    if (clicked && !disabled) {
        sound::Play(sound::Effect::Click);
    }
    return clicked && !disabled;
}

// Control "segmentado": varias opciones en una píldora; la elegida resalta.
bool Segmented(const char* id, const char* const* labels, int count, int* index) {
    const float height = ImGui::GetFrameHeight() * 1.05f;
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
    const float r = std::min(height * 0.5f, Rounding() * 0.55f);
    draw->AddRectFilled(pos, max, Col(theme::WithAlpha(p.field, hovered ? 1.5f : 1.0f)), r);
    draw->AddRect(pos, max, Col(p.border, 0.55f), r, 0, 1.0f);
    const float segment = width / static_cast<float>(count);
    const float pad = std::max(2.0f, height * 0.1f);
    const ImVec2 pillMin(pos.x + segment * t + pad, pos.y + pad);
    const ImVec2 pillMax(pillMin.x + segment - pad * 2.0f, max.y - pad);
    draw->AddRectFilled(pillMin, pillMax, Col(theme::Accent(1.0f)), std::max(0.0f, r - pad));
    for (int i = 0; i < count; ++i) {
        const ImVec2 sMin(pos.x + segment * static_cast<float>(i), pos.y);
        const ImVec2 sMax(sMin.x + segment, max.y);
        const float selected = std::clamp(1.0f - std::fabs(t - static_cast<float>(i)), 0.0f, 1.0f);
        CenteredText(draw, ImGui::GetFont(), sMin, sMax, Col(Mix(p.text, p.onAccentText, selected)), Tr(labels[i]));
    }
    return changed;
}

void FlushRow();  // (option rows, below: ends the last row before something else)

// Encabezado de sección: texto chico y tenue, un contador y una línea fina.
// (El ícono se ignora: las secciones son discretas a propósito.)
void SectionHeader(const char* icon, const char* text, int count = -1) {
    FlushRow();
    (void)icon;
    text = Tr(text);
    ImGui::Dummy(ImVec2(0.0f, Fs() * 0.5f));
    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImFont* medium = theme::GetFonts().medium;
    const float size = medium->FontSize * 0.82f;
    draw->AddText(medium, size, pos, Col(p.textDim), text);
    float x = pos.x + medium->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    if (count >= 0) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%d", count);
        const ImVec2 cs = ImGui::CalcTextSize(buffer);
        const float padX = Fs() * 0.4f;
        const ImVec2 cMin(x + Fs() * 0.5f, pos.y + (size - cs.y) * 0.5f - 1.0f);
        const ImVec2 cMax(cMin.x + cs.x + padX * 2.0f, cMin.y + cs.y + 2.0f);
        draw->AddRectFilled(cMin, cMax, Col(p.field), (cMax.y - cMin.y) * 0.5f);
        draw->AddText(ImVec2(cMin.x + padX, cMin.y + 1.0f), Col(p.textDim), buffer);
        x = cMax.x;
    }
    const float lineY = std::floor(pos.y + size * 0.55f);
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    if (right > x + Fs()) {
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(ImVec2(x + Fs() * 0.7f, lineY), ImVec2(right, lineY + 1.0f), Col(p.border, 0.7f));
        for (int i = start; i < draw->VtxBuffer.Size; ++i) {
            ImDrawVert& v = draw->VtxBuffer[i];
            const float tt = std::clamp((v.pos.x - x) / std::max(1.0f, right - x), 0.0f, 1.0f);
            const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
            v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(a) * (1.0f - tt)) << IM_COL32_A_SHIFT);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, size + Fs() * 0.45f));
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
    FlushRow();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    const float pad = Fs() * 0.9f;
    const GroupCard card = g_groupStack.back();
    g_groupStack.pop_back();
    const ImVec2 end(card.start.x + card.width, ImGui::GetItemRectMax().y + pad);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSetCurrent(0);
    const theme::Palette& p = theme::P();
    const float r = Rounding() * 0.75f;
    draw->AddRectFilled(card.start, end, Col(p.surface), r);
    draw->AddRect(card.start, end, Col(p.border, 0.55f), r, 0, 1.0f);
    draw->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(card.start.x, end.y + Fs() * 0.6f));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
}

// Una fila "etiqueta ........ control" dentro de una tarjeta.
// --- Option rows: the label on the left, the control on the right ------------------
// The label column has a fixed width. A long label (Spanish and Portuguese
// are longer than English) WRAPS inside it instead of running under the
// control, and its hint goes below it. The row is as tall as its label: the
// next item starts under whichever is lower, the label or the control.
float g_rowBottom = -1.0f;            // where the last label ended (window-local Y)
ImDrawList* g_rowWindow = nullptr;    // in which window (its draw list identifies it)

// Moves the cursor below the last row's label if the label is the taller one.
void FlushRow() {
    if (g_rowBottom >= 0.0f && ImGui::GetWindowDrawList() == g_rowWindow && ImGui::GetCursorPosY() < g_rowBottom) {
        ImGui::SetCursorPosY(g_rowBottom);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
    g_rowBottom = -1.0f;
}

void BeginRow(const char* label, const char* hint, float columnFontSizes) {
    FlushRow();
    const float fs = ImGui::GetFontSize();
    const float column = fs * columnFontSizes;
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(start.x + column - fs * 0.7f);
    ImGui::TextUnformatted(Tr(label));
    if (hint != nullptr) {
        ImGui::TextDisabled("%s", Tr(hint));
    }
    ImGui::PopTextWrapPos();
    g_rowBottom = ImGui::GetCursorPosY();
    g_rowWindow = ImGui::GetWindowDrawList();
    ImGui::SetCursorPos(ImVec2(start.x + column, start.y));  // the control, at the top of the row
}

void RowLabel(const char* label, const char* hint = nullptr) {
    BeginRow(label, hint, 15.0f);
}

void KeybindButton(const char* id, int key, bool capturing, CaptureTarget target, Module* module, int* value = nullptr) {
    const theme::Palette& p = theme::P();
    const std::string label = capturing ? std::string(Tr("Press a key...")) : keys::Name(key);
    ImGui::PushID(id);
    const ImVec2 size(Fs() * 9.0f, ImGui::GetFrameHeight());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##key", size)) {
        g_capture = capturing ? CaptureTarget::None : target;
        g_captureModule = capturing ? nullptr : module;
        g_captureValue = capturing ? nullptr : value;
        sound::Play(sound::Effect::Click);
    }
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 18.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float r = std::min(size.y * 0.5f, Rounding() * 0.55f);
    if (capturing) {
        // Parpadeo suave mientras espera la tecla.
        const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        draw->AddRectFilled(pos, max, Col(theme::Accent(0.35f + 0.35f * pulse)), r);
    } else {
        draw->AddRectFilled(pos, max, Col(theme::WithAlpha(p.field, 1.0f + 0.9f * hover)), r);
        draw->AddRect(pos, max, Col(theme::WithAlpha(p.border, 0.55f + 0.45f * hover)), r, 0, 1.0f);
    }
    icons::Draw(draw, icons::kKey, ImVec2(pos.x + size.y * 0.6f, pos.y + size.y * 0.5f), Fs() * 0.95f, Col(p.textDim));
    CenteredText(draw, ImGui::GetFont(), ImVec2(pos.x + size.y * 0.6f, pos.y), max,
                 Col(capturing ? p.onAccentText : p.text), label.c_str());
    if (hovered) {
        ImGui::SetTooltip("%s", Tr("Click: change the key. Right click: remove it.\nWhile waiting: Esc cancels, Backspace removes it."));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && target == CaptureTarget::Module && module != nullptr) {
        module->SetKeybind(keys::kNone);
        config::MarkDirty();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && target == CaptureTarget::Value && value != nullptr) {
        *value = keys::kNone;
        config::MarkDirty();
    }
    ImGui::PopID();
}

// Caja de búsqueda con lupa.
void SearchField(float width) {
    const theme::Palette& p = theme::P();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight() * 1.05f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool active = ImGui::GetActiveID() == ImGui::GetID("##search");
    const float r = std::min(height * 0.5f, Rounding() * 0.55f);
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), Col(theme::WithAlpha(p.field, active ? 1.6f : 1.0f)), r);
    draw->AddRect(pos, ImVec2(pos.x + width, pos.y + height), active ? Col(theme::Accent(0.7f)) : Col(p.border, 0.55f), r, 0, 1.0f);
    icons::Draw(draw, icons::kSearch, ImVec2(pos.x + height * 0.55f, pos.y + height * 0.5f), Fs() * 0.95f, Col(p.textDim));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + height, pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::SetNextItemWidth(width - height * 1.3f);
    ImGui::InputTextWithHint("##search", Tr("Search modules..."), g_search, sizeof(g_search));
    ImGui::PopStyleColor(3);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));
}

// =============================================================================
//  Tarjetas de módulos
// =============================================================================

// La "lámpara" de un módulo: un cuadrado redondeado con su ícono. Encendido,
// se ilumina con el acento (con un halo); apagado, es una superficie tenue.
void DrawLamp(ImDrawList* draw, ImVec2 center, float size, const char* icon, float on, float alpha) {
    const theme::Palette& p = theme::P();
    const float half = size * 0.5f;
    const float r = std::min(half, Rounding() * 0.6f);
    if (on > 0.01f) {
        const float g = size * 0.16f;
        draw->AddRectFilled(ImVec2(center.x - half - g, center.y - half - g), ImVec2(center.x + half + g, center.y + half + g),
                            Col(theme::Accent(0.14f * on), alpha), r + g);
    }
    draw->AddRectFilled(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                        Col(Mix(p.field, theme::Accent(1.0f), on), alpha), r);
    icons::Draw(draw, icon, center, size * 0.56f, Col(Mix(theme::WithAlpha(p.text, 0.8f), p.onAccentText, on), alpha));
}

// Chip con el nombre de una tecla (arriba a la derecha de un módulo).
float KeyChip(ImDrawList* draw, ImVec2 rightTop, int key, float alpha) {
    const theme::Palette& p = theme::P();
    const std::string name = keys::Name(key);
    const ImVec2 ks = ImGui::CalcTextSize(name.c_str());
    const float padX = Fs() * 0.42f;
    const ImVec2 kMax(rightTop.x, rightTop.y + ks.y + Fs() * 0.3f);
    const ImVec2 kMin(kMax.x - ks.x - padX * 2.0f, rightTop.y);
    draw->AddRectFilled(kMin, kMax, Col(p.field, alpha), Fs() * 0.3f);
    draw->AddRect(kMin, kMax, Col(p.border, alpha * 0.6f), Fs() * 0.3f, 0, 1.0f);
    draw->AddText(ImVec2(kMin.x + padX, kMin.y + Fs() * 0.15f), Col(p.textDim, alpha), name.c_str());
    return kMax.x - kMin.x;
}

// El texto de estado bajo el nombre: encendido / apagado / no disponible / qué hace un clic.
const char* ModuleStatus(const Module& module, bool hovered) {
    if (!Manager::IsAvailable(module)) return Tr("Not available in this version");
    if (hovered) return module.IsEnabled() ? Tr("Click: turn off") : Tr("Click: turn on");
    return module.IsEnabled() ? Tr("Encendido") : Tr("Apagado");
}

// El fondo de un módulo (bloque o fila). Encendido: una luz cálida que baja
// desde arriba. Elegido (con su panel de opciones abierto): borde del acento.
void ModuleBackground(ImDrawList* draw, ImVec2 min, ImVec2 max, float r, float hover, float on, bool selected, float alpha) {
    const theme::Palette& p = theme::P();
    draw->AddRectFilled(min, max, Col(Mix(p.surface, p.surfaceHover, hover), alpha), r);
    if (on > 0.01f) {
        const int start = draw->VtxBuffer.Size;
        draw->AddRectFilled(min, max, Col(theme::Accent(0.13f * on), alpha), r);
        glass::VerticalFade(draw, start, min.y, max.y, 1.0f, 0.15f);
    }
    const ImVec4 border = selected ? theme::Accent(0.9f) : Mix(theme::WithAlpha(p.border, 0.55f + 0.35f * hover), theme::Accent(0.45f), on * 0.6f);
    draw->AddRect(min, max, Col(border, alpha), r, 0, selected ? 1.6f : 1.0f);
}

// Abre (o cierra, si ya estaba) el panel de opciones de un módulo.
void ToggleDetail(Module& module) {
    g_detail = g_detail == &module ? nullptr : &module;
    sound::Play(sound::Effect::Click);
}

// Entrada escalonada: cada módulo aparece un poquito después que el anterior.
float EnterAnim(int index) {
    if (!AnimOn()) return 1.0f;
    return EaseOut(static_cast<float>((MsSince(g_pageChangedAt) - index * 25.0) / 240.0));
}

// --- Vista "bloques" -------------------------------------------------------------
void ModuleCard(Module& module, float width, float height, int index) {
    ImGui::PushID(module.Id().c_str());
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float enter = EnterAnim(index);
    const float enterOffset = (1.0f - enter) * fs * 1.0f;

    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(width, height));
    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 14.0f);
    const float on = Animate(ImGui::GetItemID() + 1, module.IsEnabled() ? 1.0f : 0.0f, 10.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = Rounding() * 0.8f;
    const ImVec2 cMin(pos.x, pos.y + enterOffset);
    const ImVec2 cMax(pos.x + width, pos.y + height + enterOffset);
    const float alpha = enter;
    const Press press = BeginPress(cMin, cMax, 0.025f);
    ModuleBackground(draw, cMin, cMax, r, hover, on, g_detail == &module, alpha);

    const float pad = fs * 0.95f;
    const float lamp = fs * 2.4f;
    DrawLamp(draw, ImVec2(cMin.x + pad + lamp * 0.5f, cMin.y + pad + lamp * 0.5f), lamp, ModuleIcon(module), on, alpha);
    if (module.Keybind() != keys::kNone) {
        KeyChip(draw, ImVec2(cMax.x - pad, cMin.y + pad), module.Keybind(), alpha);
    }

    // Nombre y estado, abajo a la izquierda.
    ImFont* medium = theme::GetFonts().medium;
    const float statusY = cMax.y - pad - fs;
    const float nameY = statusY - medium->FontSize - fs * 0.1f;
    const float toggleHeight = fs * 1.45f;
    const float toggleWidth = std::floor(std::floor(toggleHeight * 0.78f) * 1.85f);
    draw->PushClipRect(cMin, ImVec2(cMax.x - pad - toggleWidth - fs * 0.4f, cMax.y), true);
    FitText(draw, medium, cMin.x + pad, nameY + medium->FontSize, cMax.x - pad - toggleWidth - fs * 0.4f - (cMin.x + pad),
            Col(p.text, alpha), module.DisplayName(), 2);
    draw->AddText(ImVec2(cMin.x + pad, statusY), Col(module.IsEnabled() && !hovered ? theme::Accent(0.95f) : p.textDim, alpha),
                  ModuleStatus(module, hovered));
    draw->PopClipRect();

    // Interruptor abajo a la derecha; el engranaje aparece al pasar el mouse.
    ImGui::SetCursorScreenPos(ImVec2(cMax.x - pad - toggleWidth, cMax.y - pad - toggleHeight + fs * 0.2f));
    bool enabled = module.IsEnabled();
    if (Toggle("##toggle", &enabled, toggleHeight)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }
    const bool overToggle = ImGui::IsItemHovered();
    bool overGear = false;
    bool openOptions = false;
    if (hover > 0.05f || g_detail == &module) {
        const float gear = fs * 1.6f;
        const ImVec2 gearPos(cMax.x - pad - gear + fs * 0.25f,
                             module.Keybind() != keys::kNone ? cMin.y + pad + fs * 1.5f : cMin.y + pad - fs * 0.2f);
        ImGui::SetCursorScreenPos(gearPos);
        openOptions = ImGui::InvisibleButton("##options", ImVec2(gear, gear));
        overGear = ImGui::IsItemHovered();
        const float gh = Animate(ImGui::GetItemID(), overGear ? 1.0f : 0.0f, 18.0f);
        const ImVec2 gc(gearPos.x + gear * 0.5f, gearPos.y + gear * 0.5f);
        if (gh > 0.01f) draw->AddCircleFilled(gc, gear * 0.5f, Col(p.surfaceHover, gh * alpha), 24);
        icons::Draw(draw, icons::kSettings, gc, gear * 0.62f, Col(theme::WithAlpha(p.text, 0.45f + 0.55f * std::max(gh, hover * 0.6f)), alpha));
    }

    EndPress(press, theme::WithAlpha(p.text, 0.08f), !overGear && !overToggle);
    if (overGear) {
        ImGui::SetTooltip("%s", Tr("Options and key"));
    } else if (hovered && !overToggle) {
        ImGui::SetTooltip(Tr("%s\n\n%s"), module.DisplayDescription(), Tr("Click: turn on or off · Gear or right click: options"));
    }
    if (clicked && !overGear) {
        Manager::SetEnabled(module, !module.IsEnabled(), /*notify=*/false);
        sound::Play(module.IsEnabled() ? sound::Effect::ToggleOn : sound::Effect::ToggleOff);
        config::MarkDirty();
    }
    if (openOptions || rightClicked) {
        ToggleDetail(module);
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
}

// --- Vista "lista" (apilados) ------------------------------------------------------
void ModuleRow(Module& module, float width, int index) {
    ImGui::PushID(module.Id().c_str());
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    const float height = fs * 3.1f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float enter = EnterAnim(index);
    const float enterOffset = (1.0f - enter) * fs * 0.8f;

    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, height));
    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 14.0f);
    const float on = Animate(ImGui::GetItemID() + 1, module.IsEnabled() ? 1.0f : 0.0f, 10.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = Rounding() * 0.6f;
    const ImVec2 cMin(pos.x + enterOffset, pos.y);
    const ImVec2 cMax(pos.x + width + enterOffset, pos.y + height);
    const float alpha = enter;
    const Press press = BeginPress(cMin, cMax, 0.015f);
    ModuleBackground(draw, cMin, cMax, r, hover, on, g_detail == &module, alpha);

    const float pad = fs * 0.75f;
    const float lamp = fs * 1.95f;
    DrawLamp(draw, ImVec2(cMin.x + pad + lamp * 0.5f, cMin.y + height * 0.5f), lamp, ModuleIcon(module), on, alpha);

    // A la derecha: tecla, engranaje e interruptor.
    const float toggleHeight = fs * 1.45f;
    const float toggleWidth = std::floor(std::floor(toggleHeight * 0.78f) * 1.85f);
    float right = cMax.x - pad - toggleWidth;
    ImGui::SetCursorScreenPos(ImVec2(right, cMin.y + (height - toggleHeight) * 0.5f));
    bool enabled = module.IsEnabled();
    if (Toggle("##toggle", &enabled, toggleHeight)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }
    const bool overToggle = ImGui::IsItemHovered();
    const float gear = fs * 1.6f;
    right -= gear + fs * 0.4f;
    ImGui::SetCursorScreenPos(ImVec2(right, cMin.y + (height - gear) * 0.5f));
    const bool openOptions = ImGui::InvisibleButton("##options", ImVec2(gear, gear));
    const bool overGear = ImGui::IsItemHovered();
    const float gh = Animate(ImGui::GetItemID(), overGear ? 1.0f : 0.0f, 18.0f);
    const ImVec2 gc(right + gear * 0.5f, cMin.y + height * 0.5f);
    if (gh > 0.01f) draw->AddCircleFilled(gc, gear * 0.5f, Col(p.surfaceHover, gh * alpha), 24);
    icons::Draw(draw, icons::kSettings, gc, gear * 0.62f, Col(theme::WithAlpha(p.text, 0.35f + 0.65f * std::max(gh, hover * 0.5f)), alpha));
    if (module.Keybind() != keys::kNone) {
        right -= fs * 0.5f;
        const ImVec2 ks = ImGui::CalcTextSize(keys::Name(module.Keybind()).c_str());
        right -= KeyChip(draw, ImVec2(right, cMin.y + (height - ks.y - fs * 0.3f) * 0.5f), module.Keybind(), alpha);
    }

    // Nombre y descripción (recortada) en el medio.
    ImFont* medium = theme::GetFonts().medium;
    const float textX = cMin.x + pad * 2.0f + lamp;
    const float blockH = medium->FontSize + fs * 1.05f;
    const float textY = cMin.y + (height - blockH) * 0.5f;
    draw->PushClipRect(cMin, ImVec2(right - fs * 0.6f, cMax.y), true);
    FitText(draw, medium, textX, textY + medium->FontSize, right - fs * 0.6f - textX, Col(p.text, alpha),
            module.DisplayName(), 1);
    const char* second = !Manager::IsAvailable(module) ? Tr("Not available in this version") : module.DisplayDescription();
    draw->AddText(ImVec2(textX, textY + medium->FontSize + fs * 0.05f), Col(p.textDim, alpha), second);
    draw->PopClipRect();

    EndPress(press, theme::WithAlpha(p.text, 0.08f), !overGear && !overToggle);
    if (overGear) {
        ImGui::SetTooltip("%s", Tr("Options and key"));
    } else if (hovered && !overToggle) {
        ImGui::SetTooltip("%s", Tr("Click: turn on or off · Gear or right click: options"));
    }
    if (clicked) {
        Manager::SetEnabled(module, !module.IsEnabled(), /*notify=*/false);
        sound::Play(module.IsEnabled() ? sound::Effect::ToggleOn : sound::Effect::ToggleOff);
        config::MarkDirty();
    }
    if (openOptions || rightClicked) {
        ToggleDetail(module);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + fs * 0.45f));
    ImGui::Dummy(ImVec2(width, 0.0f));
    ImGui::PopID();
}

// Los módulos de una categoría, en bloques o en lista (Apariencia > Vista de
// módulos, o los botones de arriba). Devuelve cuántos dibujó. "index" lleva
// la cuenta global para la aparición escalonada.
int ModuleGrid(Category category, bool withHeader, int& index) {
    std::array<Module*, 64> matches{};
    std::size_t count = 0;
    for (const auto& module : Manager::All()) {
        if (module->GetCategory() != category || count == matches.size()) {
            continue;
        }
        if (ContainsInsensitive(module->DisplayName(), g_search) || ContainsInsensitive(module->DisplayDescription(), g_search) ||
            ContainsInsensitive(module->Name(), g_search)) {
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
    const float available = ImGui::GetContentRegionAvail().x;
    if (theme::Get().moduleView == 1) {
        for (std::size_t i = 0; i < count; ++i) {
            ModuleRow(*matches[i], available, index++);
        }
        return static_cast<int>(count);
    }
    const float gap = fs * 0.75f;
    const int columns = std::max(1, static_cast<int>((available + gap) / (fs * 12.0f + gap)));
    const float width = std::floor((available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const float height = fs * 7.4f;
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
    ImGui::TextUnformatted(Tr(title));
    ImGui::PopFont();
    if (subtitle != nullptr) {
        ImGui::TextDisabled("%s", Tr(subtitle));
    }
    ImGui::Dummy(ImVec2(0, Fs() * 0.2f));
}

void RefreshProfiles() {
    g_profiles = config::ListProfiles();
    g_profilesDirty = false;
}


// Botón cuadrado solo con ícono (por ejemplo, la vista bloques / lista).
bool IconToggle(const char* id, const char* icon, bool selected, const char* tooltip) {
    const theme::Palette& p = theme::P();
    const float size = ImGui::GetFrameHeight() * 1.05f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 18.0f);
    const float sel = Animate(ImGui::GetItemID() + 1, selected ? 1.0f : 0.0f, 16.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size, pos.y + size);
    const Press press = BeginPress(pos, max, 0.08f);
    const float r = std::min(size * 0.5f, Rounding() * 0.55f);
    draw->AddRectFilled(pos, max, Col(Mix(theme::WithAlpha(p.field, 1.0f + 0.8f * hover), theme::Accent(0.22f), sel)), r);
    draw->AddRect(pos, max, Col(Mix(theme::WithAlpha(p.border, 0.55f), theme::Accent(0.8f), sel)), r, 0, 1.0f);
    icons::Draw(draw, icon, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.5f,
                Col(Mix(theme::WithAlpha(p.text, 0.7f), theme::Accent(1.0f), sel)));
    EndPress(press, theme::WithAlpha(p.text, 0.14f));
    if (hovered && tooltip != nullptr) ImGui::SetTooltip("%s", Tr(tooltip));
    if (clicked) sound::Play(sound::Effect::Click);
    return clicked;
}

void DrawModuleDetail(Module& module);
int CountModules(Category c);

// The categories as a row of pills (they used to be sidebar entries).
void CategoryPills() {
    const float fs = Fs();
    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto pill = [&](const char* label, Category c) {
        label = Tr(label);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 size(std::floor(ts.x + fs * 1.7f), std::floor(ImGui::GetFrameHeight()));
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(c) + 100);
        const bool clicked = ImGui::InvisibleButton("##pill", size);
        const bool hovered = ImGui::IsItemHovered();
        const float on = Animate(ImGui::GetItemID(), g_categoryFilter == c ? 1.0f : 0.0f, 16.0f);
        const ImVec2 max(pos.x + size.x, pos.y + size.y);
        const Press press = BeginPress(pos, max, 0.06f);
        ImGui::PopID();
        HoverFeedback(hovered);
        const float r = size.y * 0.5f;
        draw->AddRectFilled(pos, max, Col(Mix(theme::WithAlpha(p.field, hovered ? 1.6f : 1.0f), theme::Accent(1.0f), on)), r);
        if (on < 0.99f) {
            draw->AddRect(pos, max, Col(p.border, 0.55f * (1.0f - on)), r, 0, 1.0f);
        }
        CenteredText(draw, ImGui::GetFont(), pos, max, Col(Mix(p.text, p.onAccentText, on)), label);
        EndPress(press, theme::WithAlpha(Mix(p.text, p.onAccentText, on), 0.18f));
        if (clicked && g_categoryFilter != c) {
            g_categoryFilter = c;
            g_pageChangedAt = perf::Now();
            sound::Play(sound::Effect::Click);
        }
        ImGui::SameLine(0.0f, fs * 0.4f);
    };
    pill("All", Category::Count);
    for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
        const auto c = static_cast<Category>(i);
        if (CountModules(c) > 0) {
            pill(modules::CategoryName(c), c);
        }
    }
    ImGui::NewLine();
}

void DrawModulesPage() {
    const float fs = Fs();
    // Encabezado: título a la izquierda; búsqueda, vista y HUD a la derecha.
    const float headerY = ImGui::GetCursorPosY();
    const float contentRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    PageHeader("Modules", "Everything Luma can do.");
    const float afterHeader = ImGui::GetCursorPosY();
    const float buttonH = ImGui::GetFrameHeight() * 1.05f;
    const char* hudLabel = Tr("Edit HUD");
    const float hudWidth = ImGui::CalcTextSize(hudLabel).x + fs * 3.2f;
    const float searchWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.3f, fs * 9.0f, fs * 16.0f);
    const float rightWidth = searchWidth + fs * 0.5f + buttonH * 2.0f + fs * 0.3f + fs * 0.5f + hudWidth;
    float x = contentRight - rightWidth;
    ImGui::SetCursorPos(ImVec2(x, headerY + fs * 0.2f));
    SearchField(searchWidth);
    x += searchWidth + fs * 0.5f;
    ImGui::SetCursorPos(ImVec2(x, headerY + fs * 0.2f));
    int& view = theme::Get().moduleView;
    if (IconToggle("##viewgrid", icons::kAll, view == 0, "Blocks") && view != 0) {
        view = 0;
        g_pageChangedAt = perf::Now();
        config::MarkDirty();
    }
    ImGui::SameLine(0.0f, fs * 0.3f);
    if (IconToggle("##viewlist", icons::kList, view == 1, "List (stacked)") && view != 1) {
        view = 1;
        g_pageChangedAt = perf::Now();
        config::MarkDirty();
    }
    ImGui::SameLine(0.0f, fs * 0.5f);
    if (GlassButton("Edit HUD", icons::kMove, ImVec2(hudWidth, buttonH), true)) {
        hudeditor::Begin();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", Tr("Move and scale the active HUD elements."));
    }
    ImGui::SetCursorPosY(std::max(afterHeader, headerY + buttonH + fs * 0.2f) + fs * 0.3f);
    CategoryPills();
    ImGui::Dummy(ImVec2(0, fs * 0.35f));

    // La lista de módulos y, si elegiste uno, su panel de opciones a la derecha.
    const float gap = fs * 0.9f;
    const float panelWidth = g_detail != nullptr ? std::clamp(ImGui::GetContentRegionAvail().x * 0.42f, fs * 20.0f, fs * 27.0f) : 0.0f;
    const float listWidth = ImGui::GetContentRegionAvail().x - (g_detail != nullptr ? panelWidth + gap : 0.0f);
    const ImVec2 listPos = ImGui::GetCursorScreenPos();
    const float listHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##grid", ImVec2(listWidth, listHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    SmoothScroll();
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
        icons::Draw(draw, searching ? icons::kSearch : CategoryIcon(g_categoryFilter), center, fs * 3.5f, Col(p.textDim), fs * 0.16f);
        const char* line1 = searching ? Tr("No module matches") : Tr("No modules here yet");
        const char* line2 = searching ? Tr("Try another word.") : Tr("Coming in the next versions.");
        ImFont* medium = theme::GetFonts().medium;
        CenteredText(draw, medium, ImVec2(center.x - avail.x * 0.5f, center.y + fs * 2.6f),
                     ImVec2(center.x + avail.x * 0.5f, center.y + fs * 4.0f), Col(p.text), line1);
        CenteredText(draw, ImGui::GetFont(), ImVec2(center.x - avail.x * 0.5f, center.y + fs * 4.0f),
                     ImVec2(center.x + avail.x * 0.5f, center.y + fs * 5.2f), Col(p.textDim), line2);
        ImGui::Dummy(ImVec2(0, fs * 11.0f));
    }
    ImGui::EndChild();

    if (g_detail != nullptr) {
        const theme::Palette& p = theme::P();
        const ImVec2 pMin(listPos.x + listWidth + gap, listPos.y);
        const ImVec2 pMax(pMin.x + panelWidth, pMin.y + listHeight);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pMin, pMax, Col(p.surface), Rounding() * 0.8f);
        draw->AddRect(pMin, pMax, Col(p.border, 0.55f), Rounding() * 0.8f, 0, 1.0f);
        ImGui::SetCursorScreenPos(pMin);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.0f, fs * 1.0f));
        ImGui::BeginChild("##detail", ImVec2(panelWidth, listHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
        SmoothScroll();
        ui::SetLabelColumn(9.5f);
        DrawModuleDetail(*g_detail);
        ui::SetLabelColumn(15.0f);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}

// El panel de opciones de un módulo (a la derecha de la lista).
void DrawModuleDetail(Module& module) {
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lamp = fs * 3.0f;
    const float on = Animate(ImGui::GetID("##hero"), module.IsEnabled() ? 1.0f : 0.0f, 10.0f);
    DrawLamp(draw, ImVec2(pos.x + lamp * 0.5f, pos.y + lamp * 0.5f), lamp, ModuleIcon(module), on, 1.0f);

    // Cerrar el panel (arriba a la derecha).
    const float close = fs * 1.7f;
    const ImVec2 closePos(pos.x + ImGui::GetContentRegionAvail().x - close, pos.y);
    ImGui::SetCursorScreenPos(closePos);
    if (ImGui::InvisibleButton("##closedetail", ImVec2(close, close))) {
        g_detail = nullptr;
        sound::Play(sound::Effect::Click);
        return;
    }
    const float ch = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
    if (ch > 0.01f) draw->AddCircleFilled(ImVec2(closePos.x + close * 0.5f, closePos.y + close * 0.5f), close * 0.5f, Col(p.surfaceHover, ch), 24);
    icons::Draw(draw, icons::kClose, ImVec2(closePos.x + close * 0.5f, closePos.y + close * 0.5f), close * 0.5f, Col(p.textDim));

    ImFont* titleFont = theme::GetFonts().medium;
    draw->PushClipRect(pos, ImVec2(closePos.x - fs * 0.3f, pos.y + lamp + fs), true);
    draw->AddText(titleFont, titleFont->FontSize, ImVec2(pos.x + lamp + fs * 0.8f, pos.y + fs * 0.25f), Col(p.text), module.DisplayName());
    draw->AddText(ImVec2(pos.x + lamp + fs * 0.8f, pos.y + fs * 0.35f + titleFont->FontSize), Col(p.textDim),
                  modules::CategoryName(module.GetCategory()));
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + lamp + fs * 0.7f));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
    ImGui::TextUnformatted(module.DisplayDescription());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    ui::Section("General");
    bool enabled = module.IsEnabled();
    if (ui::Toggle("Encendido", &enabled)) {
        Manager::SetEnabled(module, enabled, /*notify=*/false);
        config::MarkDirty();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr("Key"));
    ImGui::SameLine(fs * 9.5f);
    const bool capturing = g_capture == CaptureTarget::Module && g_captureModule == &module;
    KeybindButton("##detail_key", module.Keybind(), capturing, CaptureTarget::Module, &module);

    ui::Section("Options");
    if (module.HasSettings() || module.IsHudElement()) {
        if (moduleoptions::Draw(module)) {
            config::MarkDirty();
        }
        if (module.IsHudElement()) {
            ImGui::Dummy(ImVec2(0, fs * 0.3f));
            if (GlassButton("Open the HUD editor", icons::kMove, ImVec2(0, 0))) {
                hudeditor::Begin();
            }
        }
    } else {
        ImGui::TextDisabled("%s", Tr("This module has no options."));
    }
}

void DrawThemePage() {
    PageHeader("Appearance", "How Luma looks: colors, glass, shape and sound.");
    theme::Settings& t = theme::Get();
    bool changed = false;

    SectionHeader(icons::kTheme, "Style");
    BeginGroupCard("style");
    RowLabel("Glass");
    static const char* const kStyles[] = {"Dark", "Claro"};
    int style = t.style == theme::Style::Light ? 1 : 0;
    if (Segmented("style", kStyles, 2, &style)) {
        t.style = style == 1 ? theme::Style::Light : theme::Style::Dark;
        changed = true;
    }
    RowLabel("Module view");
    static const char* const kViews[] = {"Blocks", "List"};
    if (Segmented("moduleview", kViews, 2, &t.moduleView)) {
        changed = true;
    }
    RowLabel("Luma color", "(the logo's ivory)");
    if (Toggle("##accentAuto", &t.accentAuto, ImGui::GetFrameHeight() * 0.95f)) {
        changed = true;
    }
    if (!t.accentAuto) {
        RowLabel("Accent color");
        changed |= ImGui::ColorEdit3("##accent", t.accent, ImGuiColorEditFlags_NoInputs);
    }
    EndGroupCard();

    SectionHeader(icons::kVisual, "Glass");
    BeginGroupCard("glass");
    RowLabel("Blur");
    changed |= ImGui::SliderFloat("##blur", &t.blur, 0.0f, 1.0f, "%.2f");
    RowLabel("Glass opacity");
    changed |= ImGui::SliderFloat("##opacity", &t.glassOpacity, 0.1f, 0.95f, "%.2f");
    RowLabel("Dim the game");
    changed |= ImGui::SliderFloat("##dim", &t.backgroundDim, 0.0f, 0.8f, "%.2f");
    RowLabel("Corners");
    changed |= ImGui::SliderFloat("##rounding", &t.rounding, 0.0f, 28.0f, "%.0f px");
    RowLabel("Font size");
    ImGui::SliderFloat("##font", &t.fontSize, 13.0f, 28.0f, "%.0f px");
    if (ImGui::IsItemDeactivatedAfterEdit()) {  // se regenera al soltar (cuesta unos ms)
        theme::RequestFontRebuild();
        changed = true;
    }
    RowLabel("Animations");
    changed |= Toggle("##anim", &t.animations, ImGui::GetFrameHeight() * 0.95f);
    EndGroupCard();

    SectionHeader(icons::kHud, "On-screen HUD");
    BeginGroupCard("hud");
    {
        render::hud::Style& h = render::hud::GetStyle();
        bool hudChanged = false;
        RowLabel("Font");
        static const char* const kFonts[] = {"Pixel", "Luma"};
        int font = static_cast<int>(h.font);
        if (Segmented("hudfont", kFonts, 2, &font)) {
            h.font = static_cast<render::hud::FontChoice>(font);
            hudChanged = true;
        }
        if (h.font == render::hud::FontChoice::Pixel) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", render::hud::PixelFontAvailable() ? Tr("(the game's)") : Tr("(not found: using Luma)"));
        }
        RowLabel("Box background");
        hudChanged |= ImGui::SliderFloat("##hudbg", &h.background, 0.0f, 1.0f, "%.2f");
        RowLabel("Box color");
        hudChanged |= ImGui::ColorEdit3("##hudbgcolor", h.backgroundColor, ImGuiColorEditFlags_NoInputs);
        RowLabel("Text shadow");
        hudChanged |= Toggle("##hudshadow", &h.textShadow, ImGui::GetFrameHeight() * 0.95f);
        RowLabel("Corners");
        hudChanged |= ImGui::SliderFloat("##hudround", &h.rounding, 0.0f, 6.0f, "%.0f");
        RowLabel("Border");
        hudChanged |= Toggle("##hudborder", &h.border, ImGui::GetFrameHeight() * 0.95f);
        RowLabel("Text color");
        hudChanged |= ImGui::ColorEdit3("##hudtext", h.text, ImGuiColorEditFlags_NoInputs);
        RowLabel("Rainbow", "(the whole HUD changes color)");
        hudChanged |= Toggle("##hudrainbow", &h.rainbow, ImGui::GetFrameHeight() * 0.95f);
        if (h.rainbow) {
            RowLabel("Rainbow speed");
            hudChanged |= ImGui::SliderFloat("##hudrainbowspeed", &h.rainbowSpeed, 0.05f, 2.0f, "%.2f");
        }
        RowLabel("Hide in menus", "(pause, inventory, chat)");
        hudChanged |= Toggle("##hudhide", &h.hideInGameMenus, ImGui::GetFrameHeight() * 0.95f);
        RowLabel("Pressed key", "(keystrokes)");
        hudChanged |= ImGui::ColorEdit3("##hudpressed", h.pressed, ImGuiColorEditFlags_NoInputs);
        RowLabel("Positions");
        if (GlassButton("Open the HUD editor", icons::kMove, ImVec2(0, 0))) {
            hudeditor::Begin();
        }
        if (GlassButton("Default style", nullptr, ImVec2(0, 0))) {
            h = render::hud::Style{};
            hudChanged = true;
        }
        if (hudChanged) {
            config::MarkDirty();
        }
    }
    EndGroupCard();

    SectionHeader(icons::kSparkle, "Sound");
    BeginGroupCard("sound");
    RowLabel("Sound effects");
    if (Toggle("##sounds", &t.sounds, ImGui::GetFrameHeight() * 0.95f)) {
        changed = true;
    }
    ImGui::BeginDisabled(!t.sounds);
    RowLabel("Volume");
    ImGui::SliderFloat("##volume", &t.soundVolume, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        sound::SetVolume(t.soundVolume);
        sound::Play(sound::Effect::ToggleOn);
        changed = true;
    }
    RowLabel("Try");
    if (GlassButton("Open", nullptr, ImVec2(0, 0))) sound::Play(sound::Effect::Open);
    ImGui::SameLine();
    if (GlassButton("Turn on", nullptr, ImVec2(0, 0))) sound::Play(sound::Effect::ToggleOn);
    ImGui::SameLine();
    if (GlassButton("Turn off", nullptr, ImVec2(0, 0))) sound::Play(sound::Effect::ToggleOff);
    ImGui::EndDisabled();
    EndGroupCard();

    ImGui::Dummy(ImVec2(0, Fs() * 0.4f));
    if (GlassButton("Reset appearance", nullptr, ImVec2(0, 0))) {
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
    PageHeader("Profiles", "A profile saves ALL the settings: modules, options, keys and appearance.");
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    const std::string active = config::ActiveProfile();
    std::string toLoad;
    std::string toDelete;

    SectionHeader(icons::kProfiles, "Your profiles", static_cast<int>(g_profiles.size()));
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
        ModuleBackground(draw, pos, max, Rounding() * 0.7f, hover, isActive ? 1.0f : 0.0f, false, 1.0f);
        const float circle = fs * 1.9f;
        const ImVec2 c(pos.x + fs * 0.8f + circle * 0.5f, pos.y + height * 0.5f);
        DrawLamp(draw, c, circle, icons::kProfiles, isActive ? 1.0f : 0.0f, 1.0f);
        ImFont* medium = theme::GetFonts().medium;
        draw->AddText(medium, medium->FontSize, ImVec2(c.x + circle * 0.5f + fs * 0.7f, pos.y + (height - medium->FontSize) * 0.5f),
                      Col(p.text), name.c_str());
        if (isActive) {
            const ImVec2 ns = medium->CalcTextSizeA(medium->FontSize, FLT_MAX, 0.0f, name.c_str());
            draw->AddText(ImVec2(c.x + circle * 0.5f + fs * 1.3f + ns.x, pos.y + (height - fs) * 0.5f),
                          Col(p.textDim), Tr("active"));
        } else {
            ImGui::SetCursorScreenPos(ImVec2(max.x - fs * 11.0f, pos.y + (height - ImGui::GetFrameHeight() * 1.1f) * 0.5f));
            if (GlassButton("Load", nullptr, ImVec2(fs * 5.0f, 0))) toLoad = name;
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

    SectionHeader(icons::kSparkle, "New profile");
    BeginGroupCard("new");
    ImGui::SetNextItemWidth(fs * 14.0f);
    ImGui::InputTextWithHint("##newprofile", Tr("Name"), g_newProfile, sizeof(g_newProfile));
    ImGui::SameLine();
    const bool valid = config::IsValidProfileName(g_newProfile);
    ImGui::BeginDisabled(!valid);
    if (GlassButton("Create (copy of current)", nullptr, ImVec2(0, ImGui::GetFrameHeight()), true) &&
        config::CreateProfile(g_newProfile)) {
        g_newProfile[0] = '\0';
        g_profilesDirty = true;
    }
    ImGui::EndDisabled();
    if (!valid && g_newProfile[0] != '\0') {
        ImGui::TextDisabled("%s", Tr("Only letters, numbers, spaces, '-' and '_' (max 32)."));
    }
    EndGroupCard();

    SectionHeader(icons::kClose, "Danger zone");
    BeginGroupCard("danger");
    if (GlassButton("Reset this profile...", nullptr, ImVec2(0, 0))) {
        ImGui::OpenPopup("##confirm_reset");
    }
    if (ImGui::BeginPopup("##confirm_reset")) {
        ImGui::Text(Tr("Reset the whole \"%s\" profile to defaults?"), active.c_str());
        if (GlassButton("Yes, reset", nullptr, ImVec2(0, 0), true)) {
            config::ResetActiveProfile();
            theme::Apply();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (GlassButton("Cancel", nullptr, ImVec2(0, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    EndGroupCard();
}

// Guarda el idioma en launcher.json (el mismo archivo que usa el launcher: así
// los dos quedan siempre en el mismo idioma).
void SaveLanguage(i18n::Language language) {
    const std::filesystem::path path = paths::DataDir() / L"launcher.json";
    nlohmann::json data;
    {
        std::ifstream file(path);
        if (file) data = nlohmann::json::parse(file, nullptr, false);
    }
    if (!data.is_object()) data = nlohmann::json::object();
    data["language"] = i18n::LanguageCode(language);
    const std::filesystem::path temp = paths::DataDir() / L"launcher.json.tmp";
    std::ofstream(temp, std::ios::trunc) << data.dump(2);
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void DrawSettingsPage() {
    PageHeader(i18n::T("settings.title"), i18n::T("settings.subtitle"));
    SectionHeader(icons::kAll, i18n::T("settings.language"));
    BeginGroupCard("language");
    RowLabel(i18n::T("settings.language"));
    static const char* labels[i18n::kLanguageCount];
    for (int i = 0; i < i18n::kLanguageCount; ++i) labels[i] = i18n::LanguageName(static_cast<i18n::Language>(i));
    int language = static_cast<int>(i18n::CurrentLanguage());
    if (Segmented("language", labels, i18n::kLanguageCount, &language)) {
        i18n::SetLanguage(static_cast<i18n::Language>(language));
        SaveLanguage(static_cast<i18n::Language>(language));
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", i18n::T("settings.language_hint"));
    ImGui::PopTextWrapPos();
    EndGroupCard();

    SectionHeader(icons::kKey, "Luma keys");
    BeginGroupCard("keys");
    RowLabel("Open / close the menu");
    KeybindButton("##menukey", config::g_menuKey.load(), g_capture == CaptureTarget::MenuKey, CaptureTarget::MenuKey, nullptr);
    RowLabel("Unload Luma");
    KeybindButton("##unloadkey", config::g_unloadKey.load(), g_capture == CaptureTarget::UnloadKey,
                  CaptureTarget::UnloadKey, nullptr);
    RowLabel("F1 also hides Luma's HUD");
    if (Toggle("##f1hud", &theme::Get().f1HidesHud, ImGui::GetFrameHeight() * 0.95f)) config::MarkDirty();
    RowLabel("Reopen where you left it (s)");
    if (ImGui::SliderFloat("##reopen", &theme::Get().reopenSeconds, 0.0f, 15.0f, "%.1f")) config::MarkDirty();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("Esc también cierra el menú. Limitación conocida: si la tecla del menú es una "
                        "letra, escribirla en el chat abre el menú (se corrige en la Fase 7)."));
    ImGui::TextDisabled("%s", Tr("If you open the menu again within these seconds after closing it, it opens where you "
                                 "were (the same page and module options). 0 = always at the start."));
    ImGui::PopTextWrapPos();
    EndGroupCard();

    SectionHeader(icons::kAll, "Files");
    BeginGroupCard("files");
    if (GlassButton("Open the Luma folder", nullptr, ImVec2(0, 0))) {
        ShellExecuteW(nullptr, L"open", paths::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("Config, profiles and logs."));
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
        case perf::windows::Status::On:  return Tr("Activado");
        case perf::windows::Status::Off: return Tr("Desactivado");
        default:                          return Tr("Not available");
    }
}

// Una fila de consejo: título, estado (con color), explicación y un botón
// que abre la página de Configuración de Windows donde se cambia.
void TipRow(const char* title, const char* status, bool good, const char* text, const wchar_t* settingsUri) {
    title = Tr(title);
    status = Tr(status);
    text = Tr(text);
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
        if (GlassButton("Open in Windows", nullptr, ImVec2(0, 0))) {
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
    PageHeader("Benchmark", "Measures FPS in a comparable way and tests whether optimizations help.");
    const float fs = Fs();
    const perf::benchmark::State state = perf::benchmark::GetState();
    const bool idle = state == perf::benchmark::State::Idle;

    SectionHeader(icons::kChart, "New test");
    BeginGroupCard("bench");
    ImGui::BeginDisabled(!idle);
    RowLabel("Name");
    ImGui::InputTextWithHint("##benchlabel", Tr("For example: no optimizations"), g_benchLabel, sizeof(g_benchLabel));
    RowLabel("Duration");
    ImGui::SliderInt("##benchsecs", &g_benchSeconds, 10, 300, "%d s");
    RowLabel("Mode");
    static const char* const kModes[] = {"With my HUD", "Luma idle"};
    Segmented("benchmode", kModes, 2, &g_benchMode);
    RowLabel("Optimizations");
    const std::string optimizations = ActiveOptimizations();
    ImGui::TextUnformatted(optimizations.empty() ? Tr("none") : optimizations.c_str());
    ImGui::EndDisabled();

    if (idle) {
        if (GlassButton("Start (the menu closes)", icons::kChart, ImVec2(0, 0), true)) {
            const auto mode = g_benchMode == 1 ? perf::benchmark::Mode::Resting : perf::benchmark::Mode::WithHud;
            if (perf::benchmark::Start(g_benchSeconds, 5.0, g_benchLabel, mode, optimizations)) {
                notify::Push(notify::Kind::Info, Tr("Benchmark in 5 s: don't open the menu or move"));
                // Cerrar el menú desde el hilo de la ventana (ver input.h).
                render::input::RequestMenuToggle();
            }
        }
    } else {
        const char* what = state == perf::benchmark::State::Countdown ? "Starts in" : "Recording: left";
        ImGui::Text(Tr("%s %.0f s"), what, perf::benchmark::SecondsLeft());
        ImGui::SameLine();
        if (GlassButton("Cancel", icons::kClose, ImVec2(0, 0))) {
            perf::benchmark::Cancel("cancelado desde el menú");
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("Para comparar: quédate quieto en el mismo lugar y mirando lo mismo en cada prueba. "
                        "\"Luma en reposo\" no dibuja nada durante la prueba: su costo es ~0,001 ms, así que es "
                        "la referencia \"sin Luma\". Después activa UNA optimización y repite."));
    ImGui::PopTextWrapPos();
    EndGroupCard();

    const std::vector<perf::benchmark::Result> results = perf::benchmark::Results();
    SectionHeader(icons::kDiagnostics, "Results from this session", static_cast<int>(results.size()));
    if (results.empty()) {
        ImGui::TextDisabled("%s", Tr("You haven't run any test yet."));
    } else {
        constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                           ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("##results", 8, kTable)) {
            ImGui::TableSetupColumn(Tr("Name"), ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn(Tr("Mode"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn(Tr("Optimizations"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn(Tr("FPS"), ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn(Tr("1% low"), ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn(Tr("Frametime"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(Tr("Luma"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(Tr("GPU"), ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableHeadersRow();
            for (auto it = results.rbegin(); it != results.rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->label.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->mode.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(it->optimizations.c_str());
                ImGui::TableNextColumn(); ImGui::Text(Tr("%.1f"), it->averageFps);
                ImGui::TableNextColumn(); ImGui::Text(Tr("%.1f"), it->low1Fps);
                ImGui::TableNextColumn(); ImGui::Text(Tr("%.2f ms"), it->averageMs);
                ImGui::TableNextColumn(); ImGui::Text(Tr("%.3f ms"), it->lumaCostMs);
                ImGui::TableNextColumn();
                if (it->gpuMs >= 0.0) {
                    // GPU busy time and how much of the frame it is (near 100% = GPU-limited).
                    ImGui::Text("%.2f ms (%.0f%%)", it->gpuMs, it->gpuPercent);
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            ImGui::EndTable();
        }
    }
    if (GlassButton("Open the benchmarks folder", nullptr, ImVec2(0, 0))) {
        std::error_code ec;
        const std::filesystem::path dir = paths::DataDir() / L"benchmarks";
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("summary.csv = one row per test; one .csv per test with every frame."));

    // --- Consejos de Windows ---------------------------------------------------
    if (g_windowsInfoDirty) {
        g_windowsInfo = perf::windows::Read();
        g_windowsInfoDirty = false;
    }
    const perf::windows::Info& w = g_windowsInfo;
    using perf::windows::Status;
    SectionHeader(icons::kSettings, "Windows tips");
    BeginGroupCard("windows");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(Tr("Luma solo te muestra cómo están estas opciones: nunca las cambia por su cuenta. "
                           "Prueba cada una con el benchmark."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, fs * 0.3f));
    TipRow("Game Mode", StatusText(w.gameMode), w.gameMode == Status::On,
           "Windows le da prioridad al juego y evita instalar actualizaciones mientras juegas. Recomendado: "
           "activado. (Configuración > Juegos > Modo de juego.)",
           L"ms-settings:gaming-gamemode");
    TipRow("Hardware-accelerated GPU scheduling", StatusText(w.gpuScheduling), w.gpuScheduling == Status::On,
           "La tarjeta gráfica organiza su propio trabajo: puede bajar la latencia. Con GPUs y drivers recientes, "
           "suele convenir activado (pide reiniciar). (Configuración > Pantalla > Gráficos > Cambiar la "
           "configuración predeterminada de gráficos.)",
           L"ms-settings:display-advancedgraphics");
    TipRow("Optimizations for windowed games", StatusText(w.windowedOptimizations),
           w.windowedOptimizations == Status::On,
           "Minecraft Bedrock juega en ventana: esta opción le da casi la misma latencia que la pantalla completa. "
           "(Mismo lugar que la anterior.)",
           L"ms-settings:display-advancedgraphics");
    const std::string plan = TrFormat("Current plan: {}", w.powerPlan.empty() ? std::string(Tr("unknown")) : w.powerPlan);
    TipRow("Power plan", plan.c_str(), w.powerPlanIsHighPerformance,
           "En portátiles, \"Equilibrado\" puede bajar la frecuencia de la CPU. \"Alto rendimiento\" (o el modo de "
           "energía \"Mejor rendimiento\" en Windows 11) la mantiene alta. Enchufa el cargador para jugar.",
           L"ms-settings:powersleep");
    const std::string gpu = TrFormat("Now: {}", std::string(Tr(w.gpuPreference.c_str())));
    TipRow("GPU for Minecraft", gpu.c_str(), w.gpuPreference == "High performance",
           "En portátiles con dos GPU (integrada + dedicada), elige \"Alto rendimiento\" para Minecraft en "
           "Configuración > Pantalla > Gráficos. Si tu PC tiene una sola GPU, no hace falta.",
           L"ms-settings:display-advancedgraphics");
    if (GlassButton("Read again", nullptr, ImVec2(0, 0))) {
        g_windowsInfoDirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("No \"RAM cleaners\" or registry tricks: there's no evidence they work."));
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
    label = Tr(label);
    const theme::Palette& p = theme::P();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float height = Fs() * 4.2f;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ModuleBackground(draw, pos, ImVec2(pos.x + width, pos.y + height), Rounding() * 0.7f, 0.0f, 0.0f, false, 1.0f);
    ImFont* title = theme::GetFonts().title;
    draw->AddText(title, title->FontSize, ImVec2(pos.x + Fs() * 0.9f, pos.y + Fs() * 0.6f), Col(p.text), value);
    draw->AddText(ImVec2(pos.x + Fs() * 0.9f, pos.y + height - Fs() * 1.6f), Col(p.textDim), label);
}

void DrawDiagnosticsPage(const RenderInfo& info) {
    PageHeader("Diagnostics", "Game performance and Luma's cost.");
    const perf::History& history = perf::GetHistory();
    const HistoryStats stats = ComputeStats(history);
    const float fps = stats.averageMs > 0.0f ? 1000.0f / stats.averageMs : 0.0f;
    const float low1 = stats.percentile99Ms > 0.0f ? 1000.0f / stats.percentile99Ms : 0.0f;
    const perf::Summary summary = perf::LastSummary();

    SectionHeader(icons::kDiagnostics, "Now");
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
    StatTile("Luma's cost (menu closed)", buffer, tile);

    SectionHeader(icons::kChart, "Frametime");
    BeginGroupCard("graph");
    const float scaleMax = std::max(33.3f, stats.maxMs * 1.1f);
    const int offset = history.count == perf::kHistorySize ? static_cast<int>(history.next) : 0;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PlotLines("##frametime", history.frameMs.data(), static_cast<int>(history.count), offset, nullptr, 0.0f,
                     scaleMax, ImVec2(ImGui::GetContentRegionAvail().x - Fs() * 1.8f, Fs() * 6.0f));
    ImGui::PopStyleColor();
    ImGui::TextDisabled(Tr("Last %zu frames · max %.2f ms · lower = better, spikes = stutters"), history.count, stats.maxMs);
    EndGroupCard();

    SectionHeader(icons::kPerformance, "Luma's cost");
    BeginGroupCard("cost");
    if (summary.valid && summary.closedFrames > 0) {
        ImGui::Text(Tr("Menu closed (last %.0f s): %.4f ms average, %.4f ms max"), summary.seconds,
                    summary.averageCostUs / 1000.0, summary.maxCostUs / 1000.0);
    } else {
        ImGui::TextDisabled("%s", Tr("Menu closed: keep the menu closed for 30 s and open it again."));
    }
    ImGui::Text(Tr("Menu open (now, with the glass): %.3f ms average, %.3f ms max"),
                stats.openCostAvgUs / 1000.0f, stats.openCostMaxUs / 1000.0f);
    ImGui::TextDisabled(Tr("DirectX 12 · %u x %u · %s · %u buffers"), info.width, info.height, info.formatName, info.bufferCount);
    EndGroupCard();

    // Estado del SDK: qué datos del juego encontró Luma en esta versión.
    SectionHeader(icons::kWorld, "Game data (SDK)");
    BeginGroupCard("sdk");
    RowLabel("Local player");
    ImGui::TextUnformatted(!sdk::player::Available() ? Tr("Not available")
                           : sdk::player::LocalPlayer() != 0 ? "Found"
                                                             : "Move the mouse inside a world");
    RowLabel("Network (ping)");
    ImGui::TextUnformatted(sdk::network::Available() ? Tr("Available") : Tr("Not available"));
    RowLabel("Where you are");
    ImGui::TextUnformatted(sdk::network::OnServer() ? Tr("On a server") : Tr("Local world (or menu)"));
    EndGroupCard();
}

// =============================================================================
//  Barra lateral
// =============================================================================

// Una entrada de la barra lateral: ícono + texto. La "píldora" que marca la
// elegida se dibuja aparte y se desliza de una a otra (ver Sidebar).
// =============================================================================
//  Menú radial (lo primero que aparece al pulsar L) y página Closet
// =============================================================================
//  Un círculo de vidrio con 4 botones alrededor de la cara de tu skin:
//            [ Mods ]
//   [ HUD ]   (cara)   [ Closet ]
//          [ Ajustes ]
//  Mods / Closet / Ajustes abren el menú grande en esa página; HUD abre el
//  editor de HUD. Desde el menú grande, "Inicio" vuelve al radial.


// Efficiency mode (Settings > Performance): one switch, and "advanced" to
// choose what it turns off. It overrides your settings without changing them.
void DrawEfficiencySection() {
    theme::Settings& t = theme::Get();
    bool changed = false;
    ui::SetLabelColumn(13.0f);
    ui::Section("Efficiency mode");
    changed |= ui::Toggle("Efficiency mode", &t.efficiency, "(Luma as light as possible)");
    ui::Hint("Turns off what costs without being essential: animations, the glass blur (the most expensive, it "
             "uses the graphics card) and the HUD text shadows. Your settings stay as they are: turn it off and "
             "everything comes back. It doesn't touch Luma Music: the music has its own mode (Music > Styles).");
    static bool advanced = false;
    ui::Toggle("Advanced: choose what it turns off", &advanced);
    if (advanced) {
        ImGui::BeginDisabled(!t.efficiency);
        changed |= ui::Toggle("Animations", &t.effAnimations, "(menu, notifications, buttons)");
        changed |= ui::Toggle("Glass blur", &t.effBlur, "(GPU: the biggest saving with the menu open)");
        changed |= ui::Toggle("HUD text shadows", &t.effShadows, "(each text drawn once, not twice)");
        changed |= ui::Toggle("Menu sounds", &t.effSounds);
        ImGui::EndDisabled();
        ui::Hint("Measure it: run a benchmark with the mode on and off (below) and compare. What doesn't improve "
                 "your numbers isn't worth turning off.");
    }
    ui::SetLabelColumn(15.0f);
    if (changed) config::MarkDirty();
}

// =============================================================================
//  Settings: one page with tabs (General, Appearance, Profiles, Performance)
// =============================================================================
void DrawSettingsHub(const RenderInfo& info) {
    const float fs = Fs();
    static const char* const kTabs[] = {"General", "Appearance", "Profiles", "Performance"};
    if (Segmented("##settingsTabs", kTabs, 4, &g_settingsTab)) {
        g_profilesDirty = true;
        g_windowsInfoDirty = true;
        g_pageChangedAt = perf::Now();
    }
    ImGui::Dummy(ImVec2(0, fs * 0.7f));
    switch (g_settingsTab) {
        case 1: DrawThemePage(); break;
        case 2: DrawProfilesPage(); break;
        case 3:
            DrawEfficiencySection();
            ImGui::Dummy(ImVec2(0, fs * 1.0f));
            DrawDiagnosticsPage(info);
            ImGui::Dummy(ImVec2(0, fs * 1.2f));
            DrawBenchmarkPage();
            break;
        default: DrawSettingsPage(); break;
    }
}

// =============================================================================
//  Music: the song, its controls and the music settings (lyrics coming)
// =============================================================================
bool MusicButton(const char* id, ImVec2 center, float radius, const char* icon, bool primary, bool enabled,
                 ImVec4 color) {
    const theme::Palette& p = theme::P();
    ImGui::SetCursorScreenPos(ImVec2(center.x - radius, center.y - radius));
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##b", ImVec2(radius * 2.0f, radius * 2.0f)) && enabled;
    const bool hovered = enabled && ImGui::IsItemHovered();
    const float h = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 16.0f);
    const float pop = IconPop(ImGui::GetItemID(), icon);
    const Press press = BeginPress(ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius),
                                   enabled ? 0.1f : 0.0f);
    ImGui::PopID();
    HoverFeedback(hovered);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = radius * (1.0f + 0.06f * h);
    const float alpha = enabled ? 1.0f : 0.35f;
    // The icon pops in when it changes (play <-> pause): it grows from 70%
    // with a little overshoot, and fades in.
    const float popScale = 0.7f + 0.3f * pop + 0.12f * std::sin(pop * 3.14159f);
    const float popAlpha = 0.35f + 0.65f * pop;
    if (primary) {
        draw->AddCircleFilled(center, r, Col(color, alpha), 40);
        icons::Draw(draw, icon, center, r * 1.05f * popScale, Col(theme::kCharcoal, alpha * popAlpha));
    } else {
        draw->AddCircleFilled(center, r, Col(p.surface, (0.7f + 0.3f * h) * alpha), 40);
        icons::Draw(draw, icon, center, r * 0.95f * popScale, Col(Mix(p.text, color, h), alpha * popAlpha));
    }
    EndPress(press, primary ? ImVec4(0.0f, 0.0f, 0.0f, 0.18f) : theme::WithAlpha(color, 0.22f), enabled, /*round=*/true);
    if (clicked) {
        sound::Play(sound::Effect::Click);
    }
    return clicked;
}

void DrawMusicPage() {
    // While drawing this page, the widgets follow the MUSIC efficiency mode.
    g_musicUi = true;
    struct Reset {
        ~Reset() { g_musicUi = false; }
    } reset;
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    music::Settings& s = music::Get();
    const media::Track t = media::Current();
    const ImVec4 accent = music::AccentFor(s.haloColor, t, theme::Accent(1.0f));

    // The page takes the album's color, softly (like Liner): a glow from the
    // top-left and a darker bottom-right.
    {
        const ImVec2 wMin = ImGui::GetWindowPos();
        const ImVec2 wMax(wMin.x + ImGui::GetWindowWidth(), wMin.y + ImGui::GetWindowHeight());
        ImDrawList* bg = ImGui::GetWindowDrawList();
        const ImVec4 a0 = theme::WithAlpha(accent, t.artVersion != 0 ? 0.30f : 0.10f);
        const ImVec4 a1 = theme::WithAlpha(accent, t.artVersion != 0 ? 0.12f : 0.04f);
        const ImVec4 a2 = ImVec4(0.0f, 0.0f, 0.0f, 0.18f);
        bg->AddRectFilledMultiColor(wMin, wMax, Col(a0), Col(a1), Col(a2), Col(a1));
    }
    const float total = ImGui::GetContentRegionAvail().x;
    const float leftW = std::floor(std::clamp(total * 0.40f, fs * 15.0f, fs * 24.0f));
    const float gap = fs * 1.6f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float startY = ImGui::GetCursorPosY();

    // --- Left: the art, the song and the controls ---
    float y = origin.y;
    const ImVec2 artMin(origin.x, y);
    const ImVec2 artMax(origin.x + leftW, y + leftW);
    music::DrawArt(draw, artMin, artMax, s.artShape, s.artShape == 2 && s.spinArt ? music::SpinAngle() : 0.0f,
                   ImGui::GetStyle().Alpha);
    y += leftW + fs * 0.8f;
    ImFont* title = theme::GetFonts().title;
    ImFont* medium = theme::GetFonts().medium;
    if (t.present && t.title[0] != '\0') {
        draw->PushClipRect(ImVec2(origin.x, y), ImVec2(origin.x + leftW, y + fs * 6.0f), true);
        draw->AddText(title, title->FontSize, ImVec2(origin.x, y), Col(p.text), t.title);
        y += title->FontSize * 1.08f;
        draw->AddText(medium, medium->FontSize, ImVec2(origin.x, y), Col(p.text, 0.78f), t.artist);
        y += medium->FontSize * 1.15f;
        char line[240];
        const bool album = t.album[0] != '\0' && std::strcmp(t.album, t.title) != 0 && _stricmp(t.album, t.app) != 0;
        std::snprintf(line, sizeof(line), "%s%s%s", album ? t.album : "", album && t.app[0] ? "  ·  " : "", t.app);
        draw->AddText(ImVec2(origin.x, y), Col(p.textDim), line);
        draw->PopClipRect();
        y += fs * 1.9f;
    } else {
        draw->AddText(medium, medium->FontSize, ImVec2(origin.x, y), Col(p.text), Tr("Nothing is playing"));
        y += medium->FontSize * 1.2f;
        draw->PushClipRect(ImVec2(origin.x, y), ImVec2(origin.x + leftW, y + fs * 4.0f), true);
        draw->AddText(ImGui::GetFont(), fs, ImVec2(origin.x, y), Col(p.textDim), Tr("Play something on Spotify,"));
        draw->AddText(ImGui::GetFont(), fs, ImVec2(origin.x, y + fs * 1.2f), Col(p.textDim), Tr("your browser or any music app."));
        draw->PopClipRect();
        y += fs * 2.8f;
    }
    // The progress bar (click or drag to seek, if the app allows it).
    const float frac = t.duration > 0.0 ? static_cast<float>(std::clamp(t.position / t.duration, 0.0, 1.0)) : 0.0f;
    static bool seeking = false;
    static float seekFrac = 0.0f;
    static music::ProgressSpring spring;
    const float barH = std::max(3.0f, fs * 0.22f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y - fs * 0.4f));
    ImGui::InvisibleButton("##musicSeek", ImVec2(leftW, fs * 0.8f + barH));
    const bool canSeek = t.canSeek && t.duration > 0.0;
    const float mouseFrac = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / leftW, 0.0f, 1.0f);
    // A drag only once the mouse moves; a plain click travels there with the spring.
    if (canSeek && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, fs * 0.5f)) {
        seeking = true;
        seekFrac = mouseFrac;
    }
    if (canSeek && ImGui::IsItemDeactivated()) {
        spring.SeekTo(seeking ? seekFrac : mouseFrac, t.duration);
        seeking = false;
    }
    if (canSeek && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const float shown = spring.Update(frac, t.trackId, t.duration, seeking ? seekFrac : -1.0f);
    draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + leftW, y + barH), Col(p.text, 0.15f), barH * 0.5f);
    if (shown > 0.0f) {
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + leftW * shown, y + barH), Col(accent), barH * 0.5f);
        const ImVec2 dot(origin.x + leftW * shown, y + barH * 0.5f);
        if (spring.pop > 0.02f) {
            draw->AddCircle(dot, barH * (2.2f + 4.0f * (1.0f - spring.pop)), Col(accent, 0.6f * spring.pop), 20, 1.5f);
        }
        draw->AddCircleFilled(dot, barH * (1.6f + 0.7f * spring.pop), Col(accent), 20);
    }
    y += barH + fs * 0.5f;
    if (t.duration > 0.0) {
        char a[16], b[16];
        music::FormatTime(shown * t.duration, a, sizeof(a));
        music::FormatTime(t.duration, b, sizeof(b));
        draw->AddText(ImVec2(origin.x, y), Col(p.textDim), a);
        const ImVec2 bs = ImGui::CalcTextSize(b);
        draw->AddText(ImVec2(origin.x + leftW - bs.x, y), Col(p.textDim), b);
    }
    y += fs * 2.7f;
    const float cx = origin.x + leftW * 0.5f;
    const float spacing = std::min(fs * 3.4f, leftW * 0.2f);
    // Shuffle and repeat: lit when on (only if the app lets them be changed).
    auto toggleIcon = [&](const char* id, ImVec2 at, const char* icon, bool on, bool enabled, const char* badge) {
        ImGui::SetCursorScreenPos(ImVec2(at.x - fs * 0.95f, at.y - fs * 0.95f));
        ImGui::PushID(id);
        const bool clicked = ImGui::InvisibleButton("##t", ImVec2(fs * 1.9f, fs * 1.9f)) && enabled;
        const bool hovered = enabled && ImGui::IsItemHovered();
        // Turning it on or off: the dot below grows in / shrinks out.
        const float lit = Animate(ImGui::GetItemID() + 3, on ? 1.0f : 0.0f, 14.0f);
        const Press press = BeginPress(ImVec2(at.x - fs * 0.95f, at.y - fs * 0.95f), ImVec2(at.x + fs * 0.95f, at.y + fs * 0.95f),
                                       enabled ? 0.12f : 0.0f);
        ImGui::PopID();
        HoverFeedback(hovered);
        const float alpha = enabled ? 1.0f : 0.3f;
        const ImVec4 color = Mix(theme::WithAlpha(p.text, hovered ? 0.95f : 0.6f), accent, lit);
        icons::Draw(draw, icon, at, fs * 1.15f, Col(color, alpha));
        if (lit > 0.01f) draw->AddCircleFilled(ImVec2(at.x, at.y + fs * 0.9f), fs * 0.12f * lit, Col(accent, alpha * lit), 10);
        if (badge != nullptr) {
            ImFont* tiny = theme::GetFonts().regular;
            draw->AddText(tiny, tiny->FontSize * 0.72f, ImVec2(at.x + fs * 0.35f, at.y - fs * 0.75f), Col(color, alpha), badge);
        }
        EndPress(press, theme::WithAlpha(accent, 0.2f), enabled, /*round=*/true);
        if (clicked) sound::Play(sound::Effect::Click);
        return clicked;
    };
    if (toggleIcon("##shuffle", ImVec2(cx - spacing * 2.0f, y), music::kIconShuffle, t.shuffle, t.canShuffle, nullptr)) {
        media::SetShuffle(!t.shuffle);
    }
    if (MusicButton("##prev", ImVec2(cx - spacing, y), fs * 1.15f, music::kIconPrev, false, t.canPrevious, accent)) {
        media::Previous();
    }
    if (MusicButton("##play", ImVec2(cx, y), fs * 1.55f, t.playing ? music::kIconPause : music::kIconPlay, true,
                    t.canPlayPause, accent)) {
        media::PlayPause();
    }
    if (MusicButton("##next", ImVec2(cx + spacing, y), fs * 1.15f, music::kIconNext, false, t.canNext, accent)) {
        media::Next();
    }
    // Repeat goes off -> the whole list -> this song -> off.
    if (toggleIcon("##repeat", ImVec2(cx + spacing * 2.0f, y), music::kIconRepeat, t.repeat != 0, t.canRepeat,
                   t.repeat == 1 ? "1" : nullptr)) {
        media::SetRepeat(t.repeat == 0 ? 2 : t.repeat == 2 ? 1 : 0);
    }
    y += fs * 2.6f;

    // The app's volume (like Windows' volume mixer: only that app).
    if (t.volume >= 0.0f) {
        const float vw = leftW * 0.62f;
        const float vx = cx - vw * 0.5f + fs * 0.9f;
        const float vh = std::max(3.0f, fs * 0.2f);
        icons::Draw(draw, music::kIconVolume, ImVec2(vx - fs * 1.4f, y), fs * 1.05f, Col(p.text, 0.7f));
        ImGui::SetCursorScreenPos(ImVec2(vx, y - fs * 0.5f));
        ImGui::InvisibleButton("##musicVolume", ImVec2(vw - fs * 0.9f, fs));
        static float dragVolume = -1.0f;
        if (ImGui::IsItemActive()) {
            dragVolume = std::clamp((ImGui::GetIO().MousePos.x - vx) / (vw - fs * 0.9f), 0.0f, 1.0f);
            media::SetVolume(dragVolume);
        } else {
            dragVolume = -1.0f;
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float vol = dragVolume >= 0.0f ? dragVolume : t.volume;
        const float barW = vw - fs * 0.9f;
        draw->AddRectFilled(ImVec2(vx, y - vh * 0.5f), ImVec2(vx + barW, y + vh * 0.5f), Col(p.text, 0.15f), vh * 0.5f);
        draw->AddRectFilled(ImVec2(vx, y - vh * 0.5f), ImVec2(vx + barW * vol, y + vh * 0.5f), Col(p.text, 0.85f), vh * 0.5f);
        y += fs * 1.8f;
    }

    // The module's switch: the HUD element and the Halo need it on.
    Module* module = Manager::Find("now_playing");
    if (module != nullptr && !module->IsEnabled()) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        if (GlassButton("Show it on the HUD and the radial", icons::kHud, ImVec2(leftW, ImGui::GetFrameHeight() * 1.1f), true)) {
            Manager::SetEnabled(*module, true, true);
            config::MarkDirty();
        }
        y += ImGui::GetFrameHeight() * 1.1f + fs * 0.5f;
    }

    // --- Right: tabs ---
    const float rightX = origin.x + leftW + gap;
    const float rightW = total - leftW - gap;
    ImGui::SetCursorScreenPos(ImVec2(rightX, origin.y));
    static const char* const kTabs[] = {"Lyrics", "Up next", "Styles", "Halo"};
    Segmented("##musicTabs", kTabs, 4, &g_musicTab);
    const float tabsBottom = ImGui::GetCursorScreenPos().y + fs * 0.6f;
    const float rightH = std::max(fs * 10.0f, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - tabsBottom -
                                                  ImGui::GetStyle().WindowPadding.y);
    ImGui::SetCursorScreenPos(ImVec2(rightX, tabsBottom));
    ImGui::BeginChild("##musicRight", ImVec2(rightW, rightH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    SmoothScroll();
    if (g_musicTab == 0 || g_musicTab == 1) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (g_musicTab == 0) {
            music::DrawLyrics(at, ImVec2(at.x + rightW, at.y + rightH - fs * 0.2f));
        } else {
            music::DrawUpNext(at, ImVec2(at.x + rightW, at.y + rightH - fs * 0.2f));
        }
        ImGui::Dummy(ImVec2(rightW, rightH - fs * 0.4f));
    } else {
        ui::SetLabelColumn(11.0f);
        bool changed = false;
        if (g_musicTab == 2) {
            changed |= music::DrawEfficiencySettings();
            changed |= music::DrawStyleSettings();
            changed |= music::DrawAlbumColorSettings();
            changed |= music::DrawLyricsSettings();
            changed |= music::DrawVisualizerSettings();
            changed |= music::DrawSpotifySettings();
        } else {
            changed = music::DrawHaloSettings();
        }
        ui::SetLabelColumn(15.0f);
        if (changed) {
            config::MarkDirty();
        }
    }
    ImGui::EndChild();
    ImGui::SetCursorPosY(std::max(startY + (y - origin.y), ImGui::GetCursorPosY()));
    ImGui::Dummy(ImVec2(0, fs * 0.5f));
}

bool g_radial = true;  // ¿se muestra el radial (true) o el menú grande (false)?

enum class RadialItem { Mods = 0, Closet = 1, Settings = 2, Hud = 3, Count = 4 };

void OpenPage(Page page) {
    g_radial = false;
    g_page = page;
    g_detail = nullptr;
    g_categoryFilter = Category::Count;
    OnNavigate();
}

// La cabeza de tu skin en 3D (un cubo de 8x8x8 con la segunda capa encima),
// girada hacia (yaw, pitch). Se dibuja como 6+6 cuadrados con la textura de la
// skin, del más lejano al más cercano (lo mismo que el launcher, en chiquito).
void DrawHead3D(ImDrawList* draw, ImVec2 center, float size, ImTextureID tex, bool legacy, float yaw, float pitch) {
    struct V3 {
        float x, y, z;
    };
    auto rotate = [&](V3 p) {
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        V3 r{p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy};
        return V3{r.x, r.y * cp - r.z * sp, r.y * sp + r.z * cp};
    };
    const float texH = legacy ? 32.0f : 64.0f;
    const float unit = size / 10.0f;  // la cabeza mide 8 (y el sombrero 9): entra con margen
    struct Face {
        ImVec2 p[4];
        ImVec2 uv[4];
        float depth;
        float shade;
    };
    Face faces[12];
    int count = 0;
    for (int layer = 0; layer < 2; ++layer) {
        const float s = layer == 0 ? 4.0f : 4.5f;  // medio lado (la segunda capa, un poco más grande)
        const float u = layer == 0 ? 0.0f : 32.0f;
        struct Q {
            V3 c[4];
            float u, v, shade;
        };
        // (las 4 esquinas vistas desde afuera: arriba-izq, arriba-der, abajo-der, abajo-izq) y su lugar en la imagen
        const Q quads[6] = {
            {{{-s, s, s}, {s, s, s}, {s, -s, s}, {-s, -s, s}}, u + 8, 8, 0.97f},     // frente
            {{{s, s, -s}, {-s, s, -s}, {-s, -s, -s}, {s, -s, -s}}, u + 24, 8, 0.8f},  // atrás
            {{{-s, s, -s}, {-s, s, s}, {-s, -s, s}, {-s, -s, -s}}, u + 0, 8, 0.82f},  // lado derecho del personaje
            {{{s, s, s}, {s, s, -s}, {s, -s, -s}, {s, -s, s}}, u + 16, 8, 0.82f},     // lado izquierdo
            {{{-s, s, -s}, {s, s, -s}, {s, s, s}, {-s, s, s}}, u + 8, 0, 1.0f},       // arriba
            {{{-s, -s, s}, {s, -s, s}, {s, -s, -s}, {-s, -s, -s}}, u + 16, 0, 0.6f},  // abajo
        };
        for (const Q& q : quads) {
            Face& f = faces[count];
            float depth = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const V3 r = rotate(q.c[k]);
                const float persp = 40.0f / (40.0f - r.z);
                f.p[k] = ImVec2(center.x + r.x * unit * persp, center.y - r.y * unit * persp);
                depth += r.z * 0.25f;
            }
            const float cross = (f.p[1].x - f.p[0].x) * (f.p[2].y - f.p[0].y) - (f.p[1].y - f.p[0].y) * (f.p[2].x - f.p[0].x);
            if (cross <= 0.0f) continue;  // mira para atrás
            const float e = 0.02f;
            const float u0 = (q.u + e) / 64.0f, u1 = (q.u + 8 - e) / 64.0f;
            const float v0 = (q.v + e) / texH, v1 = (q.v + 8 - e) / texH;
            f.uv[0] = ImVec2(u0, v0);
            f.uv[1] = ImVec2(u1, v0);
            f.uv[2] = ImVec2(u1, v1);
            f.uv[3] = ImVec2(u0, v1);
            f.depth = depth + layer * 0.01f;
            f.shade = q.shade;
            ++count;
        }
    }
    std::sort(faces, faces + count, [](const Face& a, const Face& b) { return a.depth < b.depth; });
    const float alpha = ImGui::GetStyle().Alpha;
    for (int i = 0; i < count; ++i) {
        const int c = static_cast<int>(255.0f * faces[i].shade);
        draw->AddImageQuad(tex, faces[i].p[0], faces[i].p[1], faces[i].p[2], faces[i].p[3], faces[i].uv[0], faces[i].uv[1],
                           faces[i].uv[2], faces[i].uv[3], IM_COL32(c, c, c, static_cast<int>(255.0f * alpha)));
    }
}

void DrawRadial(float appear, float scale) {
    const theme::Palette& p = theme::P();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float fs = Fs();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##luma_radial", nullptr, kFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float outer = fs * 13.0f * scale;
        const float inner = fs * 5.4f * scale;
        const ImVec2 c(viewport->Pos.x + viewport->Size.x * 0.5f,
                       viewport->Pos.y + viewport->Size.y * 0.5f + (1.0f - appear) * fs * 1.5f);

        // El disco de vidrio (desenfoca el juego) y el círculo del centro.
        glass::Panel(draw, ImVec2(c.x - outer, c.y - outer), ImVec2(c.x + outer, c.y + outer), outer, appear);
        draw->AddCircleFilled(c, inner, Col(p.surface, 0.9f), 64);
        draw->AddCircle(c, inner, Col(p.border), 64, 1.0f);

        // The music "Halo" around the radial (render/music.h). If the mouse is
        // on it, the radial's buttons ignore it.
        bool artInCenter = false;
        const bool overHalo = music::DrawHalo(draw, c, outer, inner, appear, artInCenter);

        // ¿Sobre qué botón está el mouse? (por el ángulo y la distancia al centro)
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float dx = mouse.x - c.x;
        const float dy = mouse.y - c.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        int hovered = -1;
        if (!overHalo && dist > inner && dist < outer * 1.04f) {
            float a = std::atan2(dy, dx) * 180.0f / 3.14159265f;  // 0 = derecha, 90 = abajo
            if (a < 0.0f) a += 360.0f;
            hovered = a >= 315.0f || a < 45.0f ? static_cast<int>(RadialItem::Closet)
                      : a < 135.0f             ? static_cast<int>(RadialItem::Settings)
                      : a < 225.0f             ? static_cast<int>(RadialItem::Hud)
                                               : static_cast<int>(RadialItem::Mods);
        }
        ImGui::SetCursorScreenPos(ImVec2(c.x - outer, c.y - outer));
        const bool clicked = ImGui::InvisibleButton("##radial", ImVec2(outer * 2.0f, outer * 2.0f)) && hovered >= 0;
        HoverFeedback(hovered >= 0 && ImGui::IsItemHovered());
        static int lastHovered = -1;
        if (hovered != lastHovered && hovered >= 0) sound::Play(sound::Effect::Hover);
        lastHovered = hovered;

        struct Item {
            const char* label;
            const char* icon;
            float angle;  // grados (0 = derecha, 90 = abajo)
        };
        const Item items[4] = {
            {i18n::T("radial.mods"), icons::kAll, 270.0f},
            {i18n::T("radial.closet"), icons::kCloset, 0.0f},
            {i18n::T("radial.settings"), icons::kSettings, 90.0f},
            {i18n::T("radial.hud"), icons::kHud, 180.0f},
        };
        constexpr float kToRad = 3.14159265f / 180.0f;
        for (int i = 0; i < 4; ++i) {
            const float h = Animate(ImGui::GetID("##radial") + 10 + i, hovered == i ? 1.0f : 0.0f, 16.0f);
            const float half = 45.0f - 2.5f;  // separación entre botones
            const float a0 = (items[i].angle - half) * kToRad;
            const float a1 = (items[i].angle + half) * kToRad;
            const float ro = outer - fs * 0.35f + h * fs * 0.25f;
            const float ri = inner + fs * 0.35f;
            // el "gajo" del botón
            draw->PathArcTo(c, ro, a0, a1, 32);
            draw->PathArcTo(c, ri, a1, a0, 20);
            draw->PathFillConcave(Col(theme::Accent(1.0f), 0.05f + 0.20f * h));
            draw->PathArcTo(c, ro, a0, a1, 32);
            draw->PathArcTo(c, ri, a1, a0, 20);
            draw->PathStroke(Col(Mix(p.border, theme::Accent(1.0f), h * 0.6f)), ImDrawFlags_Closed, 1.0f);
            // ícono y nombre en el medio del gajo
            const float mid = (ro + ri) * 0.5f;
            const ImVec2 at(c.x + std::cos(items[i].angle * kToRad) * mid, c.y + std::sin(items[i].angle * kToRad) * mid);
            const float iconSize = fs * (1.7f + 0.2f * h);
            icons::Draw(draw, items[i].icon, ImVec2(at.x, at.y - fs * 0.55f), iconSize, Col(Mix(p.textDim, p.text, h)));
            ImFont* font = theme::GetFonts().medium;
            const ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, items[i].label);
            draw->AddText(font, font->FontSize, ImVec2(at.x - ts.x * 0.5f, at.y + fs * 0.75f), Col(Mix(p.textDim, p.text, h)),
                          items[i].label);
        }

        // El centro: tu cabeza en 3D, que mira hacia el mouse (o el logo, si
        // todavía no hay skin).
        bool legacy = false;
        const ImTextureID skin = skinheads::OwnSkin(legacy);
        (void)skinheads::Own();  // (sube también la cara plana, que usan otras partes)
        if (artInCenter) {
            // the album art is already in the middle
        } else if (skin != 0) {
            static float yaw = 0.0f;
            static float pitch = 0.0f;
            const float targetYaw = std::clamp(dx / outer, -1.2f, 1.2f) * 0.75f;
            const float targetPitch = std::clamp(dy / outer, -1.2f, 1.2f) * 0.55f;
            const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 10.0f);  // seguir al mouse suave
            yaw += (targetYaw - yaw) * k;
            pitch += (targetPitch - pitch) * k;
            DrawHead3D(draw, c, inner * 1.35f, skin, legacy, yaw, pitch);
        } else {
            assets::DrawLogo(draw, c, inner * 1.1f, Col(p.text));
        }
        // Debajo del círculo: cómo cerrar.
        char hint[96];
        std::snprintf(hint, sizeof(hint), i18n::T("radial.hint"), keys::Name(config::g_menuKey.load()).c_str());
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        const float hintY = media::Current().present && music::Get().active && music::Get().halo
                                ? c.y + outer + (music::Get().haloGap + 2.6f) * fs  // under the Halo's buttons
                                : c.y + outer + fs * 0.9f;
        draw->AddText(ImVec2(c.x - hs.x * 0.5f, hintY), Col(p.textDim), hint);

        if (clicked) {
            sound::Play(sound::Effect::Click);
            switch (static_cast<RadialItem>(hovered)) {
                case RadialItem::Mods: OpenPage(Page::Modules); break;
                case RadialItem::Closet: OpenPage(Page::Closet); break;
                case RadialItem::Settings: OpenPage(Page::Settings); break;
                case RadialItem::Hud: hudeditor::Begin(); break;
                default: break;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// --- Closet: tus skins (y pronto, los cosméticos) ------------------------------------
struct ClosetSkin {
    std::string id;
    std::string name;
    std::wstring file;
    bool favorite = false;
};
std::vector<ClosetSkin> g_closet;
std::string g_activeSkin;
bool g_closetDirty = true;

std::filesystem::path SkinsDir() { return paths::DataDir() / L"skins"; }

void LoadCloset() {
    g_closetDirty = false;
    g_closet.clear();
    g_activeSkin.clear();
    std::error_code ec;
    {
        std::ifstream file(SkinsDir() / L"library.json");
        const nlohmann::json root = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
        if (root.is_object() && root.contains("skins") && root["skins"].is_array()) {
            for (const nlohmann::json& j : root["skins"]) {
                if (!j.is_object()) continue;
                ClosetSkin s;
                s.id = j.value("id", std::string());
                s.name = j.value("name", std::string());
                s.file = (SkinsDir() / std::filesystem::path(j.value("file", std::string()))).native();
                s.favorite = j.value("favorite", false);
                if (!s.id.empty() && std::filesystem::exists(s.file, ec)) g_closet.push_back(std::move(s));
            }
        }
    }
    std::ifstream active(SkinsDir() / L"active.json");
    const nlohmann::json a = active ? nlohmann::json::parse(active, nullptr, false) : nlohmann::json();
    if (a.is_object()) g_activeSkin = a.value("id", std::string());
}

void Equip(const ClosetSkin& s) {
    std::error_code ec;
    std::filesystem::create_directories(SkinsDir(), ec);
    nlohmann::json j;
    j["id"] = s.id;
    std::ofstream(SkinsDir() / L"active.json", std::ios::trunc) << j.dump(2);
    g_activeSkin = s.id;
    skinpack::Write();  // la equipada queda primera en el paquete "Luma" del Vestidor
    char text[160];
    std::snprintf(text, sizeof(text), i18n::T("closet.equipped_toast"), s.name.c_str());
    notify::Push(notify::Kind::Info, text);
}

void DrawClosetPage() {
    if (g_closetDirty) LoadCloset();
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    PageHeader(i18n::T("closet.title"), i18n::T("closet.subtitle"));
    SectionHeader(icons::kCloset, i18n::T("closet.skins"), static_cast<int>(g_closet.size()));
    if (g_closet.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", i18n::T("closet.empty"));
        ImGui::PopTextWrapPos();
    } else {
        const float gap = fs * 0.7f;
        const float tileW = fs * 8.5f;
        const float tileH = fs * 11.0f;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tileW + gap)));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (std::size_t i = 0; i < g_closet.size(); ++i) {
            const ClosetSkin& s = g_closet[i];
            if (i % columns != 0) ImGui::SameLine(0.0f, gap);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 max(pos.x + tileW, pos.y + tileH);
            const bool equipped = s.id == g_activeSkin;
            ModuleBackground(draw, pos, max, Rounding() * 0.8f, 0.0f, equipped ? 1.0f : 0.0f, equipped, 1.0f);
            // la cara
            const float face = tileW * 0.55f;
            const ImVec2 fmin(pos.x + (tileW - face) * 0.5f, pos.y + fs * 0.9f);
            const ImTextureID tex = skinheads::FromFile(s.file);
            if (tex != 0) {
                draw->AddImageRounded(tex, fmin, ImVec2(fmin.x + face, fmin.y + face), ImVec2(0, 0), ImVec2(1, 1),
                                      Col(ImVec4(1, 1, 1, 1)), fs * 0.35f);
            } else {
                draw->AddRectFilled(fmin, ImVec2(fmin.x + face, fmin.y + face), Col(p.surface), fs * 0.35f);
            }
            if (s.favorite) icons::Draw(draw, icons::kSparkle, ImVec2(max.x - fs * 0.9f, pos.y + fs * 0.9f), fs, Col(p.text));
            // nombre (recortado) y botón
            std::string name = s.name;
            while (name.size() > 1 && ImGui::CalcTextSize(name.c_str()).x > tileW - fs) name.pop_back();
            const ImVec2 ns = ImGui::CalcTextSize(name.c_str());
            draw->AddText(ImVec2(pos.x + (tileW - ns.x) * 0.5f, fmin.y + face + fs * 0.5f), Col(p.text), name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(pos.x + fs * 0.6f, max.y - fs * 2.5f));
            ImGui::PushID(static_cast<int>(i));
            if (equipped) ImGui::BeginDisabled();
            if (GlassButton(equipped ? i18n::T("closet.equipped") : i18n::T("closet.equip"), nullptr,
                            ImVec2(tileW - fs * 1.2f, 0), !equipped)) {
                Equip(s);
            }
            if (equipped) ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::SetCursorScreenPos(pos);
            ImGui::Dummy(ImVec2(tileW, tileH));
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", i18n::T("closet.equip_hint"));
        ImGui::PopTextWrapPos();
    }
    SectionHeader(icons::kSparkle, i18n::T("closet.cosmetics"));
    BeginGroupCard("cosmetics");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", i18n::T("closet.cosmetics_soon"));
    ImGui::PopTextWrapPos();
    EndGroupCard();
}

// Una entrada de la barra lateral: ícono, texto y (opcional) un número a la
// derecha. La elegida se ilumina: fondo cálido tenue, texto y una rayita del
// acento a la izquierda (que se desliza de una a otra, ver Sidebar).
bool NavItem(const char* label, const char* icon, bool selected, int count = -1) {
    label = Tr(label);
    const theme::Palette& p = theme::P();
    const float height = Fs() * 2.2f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    HoverFeedback(hovered);
    const float hover = Animate(ImGui::GetItemID(), hovered && !selected ? 1.0f : 0.0f, 18.0f);
    const float active = Animate(ImGui::GetItemID() + 1, selected ? 1.0f : 0.0f, 14.0f);
    const Press press = BeginPress(pos, ImVec2(pos.x + width, pos.y + height), 0.03f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = Rounding() * 0.55f;
    if (hover > 0.01f) {
        draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), Col(p.surface, hover), r);
    }
    if (selected) {
        g_navPillTarget = pos.y - ImGui::GetWindowPos().y;
        g_navPillHeight = height;
    }
    const ImVec4 color = Mix(theme::WithAlpha(p.text, 0.72f), theme::Accent(1.0f), active);
    const float iconSize = Fs() * 1.05f;
    icons::Draw(draw, icon, ImVec2(pos.x + Fs() * 1.3f, pos.y + height * 0.5f), iconSize, Col(color));
    draw->AddText(ImVec2(pos.x + Fs() * 2.55f, pos.y + (height - Fs()) * 0.5f), Col(Mix(theme::WithAlpha(p.text, 0.85f), p.text, active)), label);
    if (count >= 0) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%d", count);
        const ImVec2 cs = ImGui::CalcTextSize(buffer);
        draw->AddText(ImVec2(pos.x + width - Fs() * 0.7f - cs.x, pos.y + (height - cs.y) * 0.5f), Col(p.textDim), buffer);
    }
    EndPress(press, theme::WithAlpha(p.text, 0.1f));
    return clicked;
}

void NavLabel(const char* text) {
    ImGui::Dummy(ImVec2(0, Fs() * 0.55f));
    const theme::Palette& p = theme::P();
    ImFont* medium = theme::GetFonts().medium;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(medium, medium->FontSize * 0.72f, ImVec2(pos.x + Fs() * 0.7f, pos.y), Col(p.textDim, 0.85f),
                                        Tr(text));
    ImGui::Dummy(ImVec2(0, medium->FontSize * 0.72f + Fs() * 0.3f));
}

// Cuántos módulos hay en una categoría (para la barra lateral).
int CountModules(Category c) {
    int n = 0;
    for (const auto& m : Manager::All()) {
        if (c == Category::Count || m->GetCategory() == c) ++n;
    }
    return n;
}

void Sidebar() {
    const theme::Palette& p = theme::P();
    const float fs = Fs();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Marca: el logo con un halo cálido, "Luma" y la versión.
    const ImVec2 brand = ImGui::GetCursorScreenPos();
    const float logo = fs * 2.2f;
    const ImVec2 lc(brand.x + fs * 0.5f + logo * 0.5f, brand.y + logo * 0.5f);
    draw->AddCircleFilled(lc, logo * 0.72f, Col(theme::Accent(0.10f)), 32);
    assets::DrawLogo(draw, lc, logo, Col(p.text));
    ImFont* title = theme::GetFonts().title;
    const float tx = brand.x + fs * 1.1f + logo;
    draw->AddText(title, title->FontSize, ImVec2(tx, brand.y + logo * 0.5f - title->FontSize * 0.62f), Col(p.text), kName);
    draw->AddText(ImVec2(tx, brand.y + logo * 0.5f + title->FontSize * 0.38f), Col(p.textDim), kVersion);
    ImGui::Dummy(ImVec2(0, logo + fs * 0.5f));

    // Dos "canales": la marca de la elegida abajo y los botones encima.
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    g_navPillTarget = -1.0f;

    // Four places: Modules (categories are pills inside), Music, Closet and
    // Settings (Appearance, Profiles, Benchmark and Diagnostics are its tabs).
    ImGui::Dummy(ImVec2(0, fs * 0.4f));
    auto go = [](Page pg) {
        g_page = pg;
        g_detail = nullptr;
        g_profilesDirty = true;
        g_windowsInfoDirty = true;
        OnNavigate();
    };
    if (NavItem("Modules", icons::kAll, g_page == Page::Modules, CountModules(Category::Count)) && g_page != Page::Modules) {
        g_categoryFilter = Category::Count;
        go(Page::Modules);
    }
    if (NavItem("Music", kMusicIcon, g_page == Page::Music) && g_page != Page::Music) {
        go(Page::Music);
    }
    if (NavItem(i18n::T("closet.title"), icons::kCloset, g_page == Page::Closet) && g_page != Page::Closet) {
        go(Page::Closet);
    }
    if (NavItem("Settings", icons::kSettings, g_page == Page::Settings) && g_page != Page::Settings) {
        go(Page::Settings);
    }

    // La marca de la elegida: un fondo cálido tenue y una rayita del acento
    // que se desliza hasta ella.
    draw->ChannelsSetCurrent(0);
    if (g_navPillTarget >= 0.0f) {
        if (g_navPillY < 0.0f || !AnimOn()) {
            g_navPillY = g_navPillTarget;
        } else {
            g_navPillY = ImLerp(g_navPillY, g_navPillTarget, std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f));
        }
        const ImVec2 win = ImGui::GetWindowPos();
        const float left = win.x + ImGui::GetStyle().WindowPadding.x;
        const float right = win.x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const ImVec2 min(left, win.y + g_navPillY);
        const ImVec2 max(right, min.y + g_navPillHeight);
        draw->AddRectFilled(min, max, Col(theme::Accent(0.13f)), Rounding() * 0.55f);
        const float barH = g_navPillHeight * 0.5f;
        draw->AddRectFilled(ImVec2(min.x, min.y + (g_navPillHeight - barH) * 0.5f), ImVec2(min.x + 3.0f, max.y - (g_navPillHeight - barH) * 0.5f),
                            Col(theme::Accent(1.0f)), 1.5f);
    }
    draw->ChannelsMerge();

    // Al pie: volver al inicio (el radial) y el perfil activo.
    const float footerH = fs * 2.2f + fs * 1.6f;
    const float footerY = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - footerH;
    if (ImGui::GetCursorScreenPos().y < footerY) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, footerY));
        if (NavItem(i18n::T("radial.home"), icons::kHome, false)) {
            g_radial = true;
            OnNavigate();
        }
        const std::string profile = TrFormat("Profile: {}", config::ActiveProfile());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + fs * 0.7f);
        ImGui::TextDisabled("%s", profile.c_str());
    }
}

}  // namespace

// =============================================================================
//  Funciones públicas
// =============================================================================

void OnOpened() {
    // QUICK REOPEN: if you closed the menu a moment ago (3 s by default), it
    // opens exactly where you were: same page, same module options, same tab,
    // same scroll (all of that is kept while it's closed; only the radial
    // "start" was forced before). Later than that, it starts at the radial.
    const double closedFor = g_closedAt == 0 ? 1.0e9 : perf::TicksToMs(perf::Now() - g_closedAt) / 1000.0;
    const bool quick = closedFor <= static_cast<double>(theme::Get().reopenSeconds);
    g_radial = quick ? g_radialWhenClosed : !hudeditor::IsActive();
    if (hudeditor::IsActive()) g_radial = false;
    g_closetDirty = true;
    g_openedAt = perf::Now();
    g_pageChangedAt = g_openedAt;
    g_closing.store(false);
    g_lastHovered = 0;
    sound::Play(sound::Effect::Open);
}

void OnClosed() {
    media::Want(media::kMenu, false);
    lyrics::Want(lyrics::kMenu, false);
    if (hudeditor::IsActive()) {
        hudeditor::End();
    }
    g_closedAt = perf::Now();
    g_radialWhenClosed = g_radial;
    g_closing.store(AnimOn());
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
    int* value = g_captureValue;
    g_capture = CaptureTarget::None;
    g_captureModule = nullptr;
    g_captureValue = nullptr;
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
        case CaptureTarget::Value:
            if (value != nullptr) {
                *value = clear ? keys::kNone : vk;
            }
            break;
        case CaptureTarget::None:
            return;
    }
    if (!clear && vk == config::g_menuKey.load() && (target == CaptureTarget::Module || target == CaptureTarget::Value)) {
        notify::Push(notify::Kind::Warning, Tr("That key also opens the menu"));
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
    } else if (AnimOn()) {
        const float t = static_cast<float>(MsSince(g_openedAt) / kOpenMs);
        appear = EaseOut(t);
        scale = 0.96f + 0.04f * EaseOutBack(t);  // crece hasta su tamaño con un leve rebote
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

    // The Music page reads the music even if the module is off.
    media::Want(media::kMenu, !g_radial && g_page == Page::Music && !closing);
    lyrics::Want(lyrics::kMenu, !g_radial && g_page == Page::Music && !closing);
    if (!g_radial && g_page == Page::Music && !closing && music::HdCoversOn()) {
        lyrics::SetHdCovers(true);
        lyrics::Want(lyrics::kCovers, true);
    }

    // Al abrir: el menú radial (Mods, Música, Closet, Ajustes).
    if (g_radial) {
        DrawRadial(appear, scale);
        return;
    }

    // Una ventana anfitriona invisible que cubre la pantalla; adentro, UNA
    // ventana de vidrio centrada con la barra lateral y el contenido.
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin("##luma_menu", nullptr, kFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const theme::Palette& p = theme::P();
        const float width = std::clamp(viewport->Size.x * 0.74f, fs * 60.0f, fs * 104.0f) * scale;
        const float height = std::clamp(viewport->Size.y * 0.78f, fs * 36.0f, fs * 64.0f) * scale;
        const float rise = (1.0f - appear) * fs * 1.2f;
        const ImVec2 origin(std::floor(viewport->Pos.x + (viewport->Size.x - width) * 0.5f),
                            std::floor(viewport->Pos.y + (viewport->Size.y - height) * 0.5f + rise));
        const ImVec2 end(origin.x + width, origin.y + height);
        const float sidebarWidth = std::floor(fs * 14.0f * scale);
        const float r = Rounding() * 1.1f;

        glass::Panel(draw, origin, end, r, appear);
        // La barra lateral: un poco más oscura, separada por una línea fina.
        draw->AddRectFilled(origin, ImVec2(origin.x + sidebarWidth, end.y), Col(ImVec4(0, 0, 0, p.text.x > 0.5f ? 0.16f : 0.04f)), r,
                            ImDrawFlags_RoundCornersLeft);
        draw->AddRectFilled(ImVec2(origin.x + sidebarWidth, origin.y + r), ImVec2(origin.x + sidebarWidth + 1.0f, end.y - r), Col(p.border, 0.6f));

        const float pad = fs * 1.1f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 0.8f, pad));
        ImGui::SetCursorScreenPos(origin);
        ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        Sidebar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Transición entre páginas: el contenido nuevo aparece y se desliza.
        float pageT = 1.0f;
        if (AnimOn() && !closing) {
            pageT = EaseOut(static_cast<float>(MsSince(g_pageChangedAt) / kPageMs));
        }
        const float slide = (1.0f - pageT) * fs * 1.5f;
        const ImVec2 mainMin(origin.x + sidebarWidth + 1.0f, origin.y);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 1.3f, pad * 1.15f));
        ImGui::SetCursorScreenPos(ImVec2(mainMin.x + slide, mainMin.y));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear * pageT);
        ImGui::BeginChild("##main", ImVec2(end.x - mainMin.x, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse |
                              (g_page == Page::Modules ? ImGuiWindowFlags_NoScrollbar : 0));
        if (g_page != Page::Modules) SmoothScroll();
        switch (g_page) {
            case Page::Theme:       g_settingsTab = 1; g_page = Page::Settings; DrawSettingsHub(info); break;
            case Page::Profiles:    g_settingsTab = 2; g_page = Page::Settings; DrawSettingsHub(info); break;
            case Page::Benchmark:
            case Page::Diagnostics: g_settingsTab = 3; g_page = Page::Settings; DrawSettingsHub(info); break;
            case Page::Settings:    DrawSettingsHub(info); break;
            case Page::Modules:     DrawModulesPage(); break;
            case Page::Closet:      DrawClosetPage(); break;
            case Page::Music:       DrawMusicPage(); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace luma::render::menu

// =============================================================================
//  Controles para las opciones de los módulos (render/ui.h)
// =============================================================================
//  Usan los widgets de este archivo, así las opciones de cada módulo se ven
//  igual que el resto del menú (y que el panel del editor de HUD).

namespace luma::render::ui {

namespace m = luma::render::menu;

namespace {

float g_labelColumn = 15.0f;  // ancho de la columna de etiquetas (en tamaños de letra)

void Row(const char* label, const char* hint) {
    m::BeginRow(label, hint, g_labelColumn);  // (see BeginRow: long labels wrap, nothing gets covered)
}

// El ancho que queda para el control a la derecha de la etiqueta.
void FillWidth() {
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 0.5f));
}

}  // namespace

void SetLabelColumn(float fontSizes) {
    g_labelColumn = fontSizes;
}

void Section(const char* title) {
    m::FlushRow();
    title = Tr(title);
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.3f));
    const theme::Palette& p = theme::P();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImFont* medium = theme::GetFonts().medium;
    draw->AddText(medium, medium->FontSize * 0.85f, pos, m::Col(p.textDim), title);
    const float tw = medium->CalcTextSizeA(medium->FontSize * 0.85f, FLT_MAX, 0.0f, title).x;
    const float lineY = pos.y + medium->FontSize * 0.45f;
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    draw->AddRectFilled(ImVec2(pos.x + tw + ImGui::GetFontSize() * 0.6f, lineY), ImVec2(right, lineY + 1.0f),
                        m::Col(p.border, 0.6f));
    ImGui::Dummy(ImVec2(0.0f, medium->FontSize * 0.85f + ImGui::GetFontSize() * 0.25f));
}

void Hint(const char* text) {
    m::FlushRow();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr(text));
    ImGui::PopTextWrapPos();
}

bool Toggle(const char* label, bool* value, const char* hint) {
    ImGui::PushID(label);
    Row(label, hint);
    const bool changed = m::Toggle("##t", value, ImGui::GetFrameHeight() * 0.95f);
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool Choice(const char* label, int* index, std::initializer_list<const char*> options) {
    ImGui::PushID(label);
    Row(label, nullptr);
    const int count = std::min(static_cast<int>(options.size()), 16);
    *index = std::clamp(*index, 0, count - 1);
    bool changed = false;
    // Las opciones en el idioma elegido.
    const char* labels[16];
    for (int i = 0; i < count; ++i) labels[i] = Tr(options.begin()[i]);
    // ¿Entran como control segmentado? (cada segmento mide 7 letras de ancho)
    std::size_t longest = 0;
    for (int i = 0; i < count; ++i) {
        longest = std::max(longest, std::strlen(labels[i]));
    }
    if (count <= 4 && longest <= 11 && ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 7.0f * count) {
        changed = m::Segmented("##s", labels, count, index);
    } else {
        FillWidth();
        const char* preview = labels[*index];
        if (ImGui::BeginCombo("##c", preview)) {
            for (int i = 0; i < count; ++i) {
                if (ImGui::Selectable(labels[i], i == *index) && i != *index) {
                    *index = i;
                    changed = true;
                    sound::Play(sound::Effect::Click);
                }
            }
            ImGui::EndCombo();
        }
    }
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool Slider(const char* label, float* value, float min, float max, const char* format) {
    ImGui::PushID(label);
    Row(label, nullptr);
    FillWidth();
    const bool changed = ImGui::SliderFloat("##v", value, min, max, Tr(format));
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool SliderInt(const char* label, int* value, int min, int max, const char* format) {
    ImGui::PushID(label);
    Row(label, nullptr);
    FillWidth();
    const bool changed = ImGui::SliderInt("##v", value, min, max, Tr(format));
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool Color(const char* label, float* rgb) {
    ImGui::PushID(label);
    Row(label, nullptr);
    const bool changed = ImGui::ColorEdit3("##c", rgb, ImGuiColorEditFlags_NoInputs);
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool ColorAlpha(const char* label, float* rgba) {
    ImGui::PushID(label);
    Row(label, nullptr);
    const bool changed =
        ImGui::ColorEdit4("##c", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool Text(const char* label, char* buffer, int size, const char* placeholder) {
    ImGui::PushID(label);
    Row(label, nullptr);
    FillWidth();
    const bool changed = ImGui::InputTextWithHint("##t", Tr(placeholder), buffer, static_cast<std::size_t>(size));
    m::FlushRow();
    ImGui::PopID();
    return changed;
}

bool Key(const char* label, int* key, const char* hint) {
    ImGui::PushID(label);
    Row(label, hint);
    const bool capturing = m::g_capture == m::CaptureTarget::Value && m::g_captureValue == key;
    m::KeybindButton("##key", *key, capturing, m::CaptureTarget::Value, nullptr, key);
    m::FlushRow();
    ImGui::PopID();
    return false;  // (the new key arrives later, when it's pressed; the config is saved then)
}

bool Button(const char* label, const char* rowLabel) {
    if (rowLabel != nullptr) {
        Row(rowLabel, nullptr);
    }
    const bool clicked = m::GlassButton(label, nullptr, ImVec2(0, 0));
    m::FlushRow();
    return clicked;
}

}  // namespace luma::render::ui
