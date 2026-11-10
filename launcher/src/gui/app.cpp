// =============================================================================
//  app.cpp - La interfaz del launcher (ver app.h)
// =============================================================================
//  CÓMO ESTÁ ARMADA
//    Una sola "ventana" de ImGui invisible que cubre todo; adentro dibujamos
//    a mano (con la "draw list" de ImGui: rectángulos, texto, íconos) y
//    usamos botones invisibles para saber dónde se hace clic. Así el diseño
//    queda exactamente como queremos, sin el aspecto típico de ImGui.
//
//    ┌──────────┬──────────────────────────────── [tema] [_] [□] [x] ┐
//    │  LUMA    │  ┌ cartel: bienvenida + JUGAR ──────────────────┐ │
//    │  Inicio  │  └──────────────────────────────────────────────┘ │
//    │  Add-ons │  ┌ Acceso rápido ─────────┐ ┌ Info del cliente ┐  │
//    │  Skins   │  └────────────────────────┘ └──────────────────┘  │
//    │  Ajustes │  ┌ franja de la marca ───────────────────────────┐ │
//    │ (perfil) │  └──────────────────────────────────────────────┘ │
//    └──────────┴───────────────────────────────────────────────────┘
//
//  ANIMACIONES: cada elemento guarda un número de 0 a 1 ("qué tan encima
//  está el mouse") que avanza un poco cada frame; con eso se mezclan los
//  colores. Son cuentas mínimas: no afectan el rendimiento.
// =============================================================================
#include "gui/app.h"

#include <shlobj.h>

#include <imgui.h>
#include <imgui_internal.h>  // ShadeVertsLinearColorGradientKeepAlpha (degradados)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/version.h"
#include "flow.h"
#include "gui/theme.h"
#include "i18n.h"
#include "render/icons.h"

namespace luma::launcher::gui {

namespace {

namespace fs = std::filesystem;
namespace icons = render::icons;
using i18n::T;

// --- Íconos propios del launcher (cuadrícula de 24x24, como los del menú) -------
constexpr const char* kIconHome = "M3.5 10.5L12 3.5l8.5 7 M5.5 9v11h13V9 M10 20v-6h4v6";
constexpr const char* kIconAddons =
    "M5 8h3.5C8 6 9 4.5 10.5 4.5S13 6 12.5 8H16v3.5c2-.5 3.5.5 3.5 2s-1.5 2.5-3.5 2V19H5z";
constexpr const char* kIconSkins =
    "M8.5 4L4 6.5l1.8 4L8 9.6V20h8V9.6l2.2.9 1.8-4L15.5 4C14.8 5.3 13.5 6 12 6S9.2 5.3 8.5 4z";
constexpr const char* kIconPlay = "F M8 5.2V18.8L18.8 12Z";
constexpr const char* kIconSun =
    "M8 12C8 9.8 9.8 8 12 8S16 9.8 16 12S14.2 16 12 16S8 14.2 8 12z M12 2.5V5 M12 19v2.5 M2.5 12H5 M19 12h2.5 "
    "M5.3 5.3l1.8 1.8 M16.9 16.9l1.8 1.8 M5.3 18.7l1.8-1.8 M16.9 7.1l1.8-1.8";
constexpr const char* kIconMoon =
    "M19.5 14.6C18.4 15.2 17.2 15.5 15.9 15.5C11.8 15.5 8.5 12.2 8.5 8.1C8.5 6.8 8.8 5.6 9.4 4.5C6 5.6 3.5 8.8 3.5 "
    "12.6C3.5 17.3 7.3 21 12 21C15.8 21 19 18.5 19.5 14.6z";
constexpr const char* kIconMinimize = "M6 12h12";
constexpr const char* kIconMaximize = "M6.5 6.5h11v11h-11z";
constexpr const char* kIconRestore = "M8.5 9.5h9v8h-9z M6.5 14.5v-8h9";
constexpr const char* kIconAlert = "M12 4L21 19.5H3z M12 10v4.5 M12 17v.5";

// --- Medidas (a 100% de escala; se multiplican por g_scale) ---------------------
constexpr float kSidebarWidth = 232.0f;
constexpr float kTitleBarHeight = 48.0f;
constexpr float kPad = 24.0f;      // margen del contenido
constexpr float kGap = 18.0f;      // separación entre tarjetas
constexpr float kRound = 14.0f;    // esquinas de las tarjetas

enum class Page { Home = 0, Addons, Skins, Settings };

// --- Estado ---------------------------------------------------------------------
HWND g_hwnd = nullptr;
float g_scale = 1.0f;
bool g_fontsDirty = false;
Page g_page = Page::Home;
float g_pageFade = 1.0f;  // 0..1: aparición del contenido al cambiar de sección
float g_fade = 1.0f;      // el alfa con que se dibuja el contenido en este frame

struct Fonts {
    ImFont* regular = nullptr;   // texto normal
    ImFont* tiny = nullptr;      // texto chico ("small" no se puede: Windows lo usa)
    ImFont* semibold = nullptr;  // títulos de tarjetas, botones
    ImFont* title = nullptr;     // "LUMA", "JUGAR"
    ImFont* hero = nullptr;      // el nombre grande del cartel
} g_fonts;

// Zonas de la barra de título que NO sirven para arrastrar (los botones).
struct Rect {
    ImVec2 min, max;
};
Rect g_noDrag[6];
int g_noDragCount = 0;

PlayFlow g_flow;
PlayState g_lastState = PlayState::Idle;
double g_minimizeAt = 0.0;  // cuándo minimizar después de JUGAR (0 = no)

// El estado del juego lo consulta un hilo aparte cada 1,5 s (listar procesos
// no es gratis: no lo hacemos en cada frame).
std::mutex g_statusMutex;
GameStatus g_status;
std::thread g_statusThread;
std::atomic<bool> g_quit{false};

// --- Config del launcher (%APPDATA%\Luma\launcher.json) ---------------------------
fs::path ConfigPath() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma" / L"launcher.json";
    }
    CoTaskMemFree(raw);
    return path;
}

void LoadConfig() {
    std::ifstream file(ConfigPath());
    if (!file) {
        return;  // primera vez: valores por defecto (tema oscuro, inglés)
    }
    const nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return;
    }
    SetTheme(json.value("theme", std::string("dark")) == "light" ? ThemeKind::Light : ThemeKind::Dark);
    i18n::SetLanguage(i18n::LanguageFromCode(json.value("language", std::string("en")).c_str()));
}

void SaveConfig() {
    const fs::path path = ConfigPath();
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    nlohmann::json json;
    json["theme"] = CurrentTheme() == ThemeKind::Light ? "light" : "dark";
    json["language"] = i18n::LanguageCode(i18n::CurrentLanguage());
    std::ofstream(path) << json.dump(2);
}

// --- Ayudas de dibujo --------------------------------------------------------------
float S(float v) { return v * g_scale; }

ImU32 C(const ImVec4& c, float alpha = 1.0f) { return Col(c, alpha * g_fade); }

float Dt() { return std::min(ImGui::GetIO().DeltaTime, 0.05f); }

// Avanza una animación de 0 a 1 (on) o de 1 a 0 (off), suavizada.
float Anim(ImGuiID id, bool on, float speed = 10.0f) {
    float& v = *ImGui::GetStateStorage()->GetFloatRef(id, 0.0f);
    v = std::clamp(v + (on ? 1.0f : -1.0f) * Dt() * speed, 0.0f, 1.0f);
    return v * v * (3.0f - 2.0f * v);  // "smoothstep": arranca y termina suave
}

ImVec2 TextSize(ImFont* font, const char* text) {
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
}

void Text(ImDrawList* d, ImFont* font, ImVec2 pos, ImU32 color, const char* text) {
    d->AddText(font, font->FontSize, pos, color, text);
}

// Texto recortado con "..." si no entra en "width".
void TextFit(ImDrawList* d, ImFont* font, ImVec2 pos, ImU32 color, const char* text, float width) {
    if (TextSize(font, text).x <= width) {
        Text(d, font, pos, color, text);
        return;
    }
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", text);
    size_t len = std::strlen(buffer);
    while (len > 1) {
        // retroceder un carácter UTF-8 completo
        do {
            --len;
        } while (len > 0 && (static_cast<unsigned char>(buffer[len]) & 0xC0) == 0x80);
        buffer[len] = '\0';
        char shown[260];
        std::snprintf(shown, sizeof(shown), "%s...", buffer);
        if (TextSize(font, shown).x <= width) {
            Text(d, font, pos, color, shown);
            return;
        }
    }
}

void Card(ImDrawList* d, ImVec2 min, ImVec2 max, float hover = 0.0f) {
    const Palette& p = P();
    d->AddRectFilled(min, max, C(Mix(p.card, p.cardHover, hover)), S(kRound));
    d->AddRect(min, max, C(p.border), S(kRound), 0, 1.0f);
}

// Rectángulo redondeado con degradado vertical (top -> bottom).
void GradientRect(ImDrawList* d, ImVec2 min, ImVec2 max, ImU32 top, ImU32 bottom, float rounding) {
    const int start = d->VtxBuffer.Size;
    d->AddRectFilled(min, max, IM_COL32_WHITE, rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(d, start, d->VtxBuffer.Size, min, ImVec2(min.x, max.y), top, bottom);
}

// Un punto de color (estado) con un halo suave.
void Dot(ImDrawList* d, ImVec2 center, const ImVec4& color) {
    d->AddCircleFilled(center, S(6.5f), C(color, 0.18f));
    d->AddCircleFilled(center, S(3.8f), C(color));
}

// Rueda girando (mientras JUGAR trabaja).
void Spinner(ImDrawList* d, ImVec2 center, float radius, ImU32 color) {
    const float t = static_cast<float>(ImGui::GetTime());
    const float start = t * 6.0f;
    d->PathArcTo(center, radius, start, start + 4.2f, 24);
    d->PathStroke(color, ImDrawFlags_None, S(2.4f));
}

// Botón invisible en un rectángulo: devuelve si se hizo clic; "hover" sale 0..1 animado.
bool Hit(const char* id, ImVec2 min, ImVec2 max, float* hover, bool enabled = true) {
    ImGui::SetCursorScreenPos(min);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
    const bool hovered = enabled && ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (hover != nullptr) {
        *hover = Anim(ImGui::GetID(id), hovered);
    }
    return enabled && clicked;
}

void GoTo(Page page) {
    if (g_page != page) {
        g_page = page;
        g_pageFade = 0.0f;
    }
}

const char* PlayerName() {
    return T("nav.guest");  // parte 2: el gamertag (del juego o de Microsoft)
}

// --- Barra de título -----------------------------------------------------------------
void DrawTitleBar(ImDrawList* d, ImVec2 display) {
    const Palette& p = P();
    g_noDragCount = 0;
    const float w = S(46.0f);
    const float h = S(34.0f);
    const float y = S(7.0f);
    float x = display.x - S(8.0f) - w;
    const bool maximized = IsZoomed(g_hwnd) != FALSE;

    struct Button {
        const char* id;
        const char* icon;
        int action;  // 0 cerrar, 1 maximizar, 2 minimizar, 3 tema
    };
    const Button buttons[] = {
        {"##close", icons::kClose, 0},
        {"##maximize", maximized ? kIconRestore : kIconMaximize, 1},
        {"##minimize", kIconMinimize, 2},
        {"##theme", CurrentTheme() == ThemeKind::Dark ? kIconSun : kIconMoon, 3},
    };
    for (const Button& b : buttons) {
        const ImVec2 min(x, y);
        const ImVec2 max(x + w, y + h);
        g_noDrag[g_noDragCount++] = {min, max};
        float hover = 0.0f;
        const bool clicked = Hit(b.id, min, max, &hover);
        const ImVec4 bg = b.action == 0 ? p.danger : p.text;
        d->AddRectFilled(min, max, Col(bg, (b.action == 0 ? 0.85f : 0.08f) * hover), S(8.0f));
        const ImVec4 iconColor = b.action == 0 ? Mix(p.textDim, ImVec4(1, 1, 1, 1), hover) : Mix(p.textDim, p.text, hover);
        icons::Draw(d, b.icon, ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), S(b.action == 3 ? 18.0f : 16.0f),
                    Col(iconColor), S(1.5f));
        if (ImGui::IsItemHovered()) {
            const char* tip = b.action == 0   ? T("title.close")
                              : b.action == 1 ? (maximized ? T("title.restore") : T("title.maximize"))
                              : b.action == 2 ? T("title.minimize")
                                              : (CurrentTheme() == ThemeKind::Dark ? T("title.theme_light")
                                                                                   : T("title.theme_dark"));
            ImGui::SetTooltip("%s", tip);
        }
        if (clicked) {
            switch (b.action) {
                case 0: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
                case 1: ShowWindow(g_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE); break;
                case 2: ShowWindow(g_hwnd, SW_MINIMIZE); break;
                case 3:
                    SetTheme(CurrentTheme() == ThemeKind::Dark ? ThemeKind::Light : ThemeKind::Dark);
                    SaveConfig();
                    break;
                default: break;
            }
        }
        x -= w + S(2.0f);
    }
}

// --- Barra lateral --------------------------------------------------------------------
void DrawSidebar(ImDrawList* d, ImVec2 display) {
    const Palette& p = P();
    const float width = S(kSidebarWidth);
    d->AddRectFilled(ImVec2(0, 0), ImVec2(width, display.y), Col(p.sidebar));
    d->AddLine(ImVec2(width, 0), ImVec2(width, display.y), Col(p.border));

    // Logo + "LUMA"
    const float logoSize = S(46.0f);
    const ImVec2 logoCenter(width * 0.5f, S(62.0f));
    icons::Draw(d, icons::kLumaLogo, logoCenter, logoSize, Col(p.text));
    const char* brand = "LUMA";
    const ImVec2 bs = TextSize(g_fonts.title, brand);
    Text(d, g_fonts.title, ImVec2(width * 0.5f - bs.x * 0.5f, logoCenter.y + logoSize * 0.5f + S(6.0f)), Col(p.text), brand);

    // Secciones
    struct Item {
        Page page;
        const char* icon;
        const char* key;
    };
    const Item items[] = {
        {Page::Home, kIconHome, "nav.home"},
        {Page::Addons, kIconAddons, "nav.addons"},
        {Page::Skins, kIconSkins, "nav.skins"},
        {Page::Settings, icons::kSettings, "nav.settings"},
    };
    float y = S(170.0f);
    const float itemH = S(46.0f);
    const float mx = S(14.0f);
    for (const Item& item : items) {
        const ImVec2 min(mx, y);
        const ImVec2 max(width - mx, y + itemH);
        float hover = 0.0f;
        char id[24];
        std::snprintf(id, sizeof(id), "##nav%d", static_cast<int>(item.page));
        if (Hit(id, min, max, &hover)) {
            GoTo(item.page);
        }
        const bool active = g_page == item.page;
        const float on = Anim(ImGui::GetID(id) + 1, active, 12.0f);
        if (on > 0.0f) {
            d->AddRectFilled(min, max, Col(p.accent, on), S(11.0f));
        }
        if (hover > 0.0f && on < 1.0f) {
            d->AddRectFilled(min, max, Col(p.text, 0.06f * hover * (1.0f - on)), S(11.0f));
        }
        const ImU32 color = Col(Mix(Mix(p.textDim, p.text, hover), p.onAccent, on));
        icons::Draw(d, item.icon, ImVec2(min.x + S(26.0f), (min.y + max.y) * 0.5f), S(20.0f), color, S(1.7f));
        Text(d, g_fonts.semibold, ImVec2(min.x + S(50.0f), (min.y + max.y) * 0.5f - g_fonts.semibold->FontSize * 0.5f),
             color, T(item.key));
        y += itemH + S(6.0f);
    }

    // Perfil (abajo)
    const float py = display.y - S(78.0f);
    d->AddLine(ImVec2(S(18.0f), py), ImVec2(width - S(18.0f), py), Col(p.border));
    const ImVec2 avatar(S(40.0f), py + S(39.0f));
    d->AddCircleFilled(avatar, S(18.0f), Col(p.card));
    d->AddCircle(avatar, S(18.0f), Col(p.border), 0, 1.0f);
    const char* name = PlayerName();
    char initial[5] = {name[0], 0};
    const ImVec2 is = TextSize(g_fonts.semibold, initial);
    Text(d, g_fonts.semibold, ImVec2(avatar.x - is.x * 0.5f, avatar.y - is.y * 0.5f), Col(p.text), initial);
    TextFit(d, g_fonts.semibold, ImVec2(S(68.0f), avatar.y - S(19.0f)), Col(p.text), name, width - S(84.0f));
    TextFit(d, g_fonts.tiny, ImVec2(S(68.0f), avatar.y + S(2.0f)), Col(p.textDim), T("nav.not_signed_in"),
            width - S(84.0f));
}

// --- Inicio -----------------------------------------------------------------------------
void DrawPlayStatus(ImDrawList* d, ImVec2 pos, float maxWidth, const PlaySnapshot& snap) {
    const Palette& p = P();
    if (snap.state == PlayState::Idle) {
        return;
    }
    const bool busy = snap.state != PlayState::Done && snap.state != PlayState::AlreadyLoaded &&
                      snap.state != PlayState::Failed;
    const ImVec2 iconCenter(pos.x + S(8.0f), pos.y + g_fonts.regular->FontSize * 0.5f);
    if (busy) {
        Spinner(d, iconCenter, S(6.5f), C(p.warning));
    } else if (snap.state == PlayState::Failed) {
        icons::Draw(d, kIconAlert, iconCenter, S(17.0f), C(p.danger), S(1.6f));
    } else {
        Dot(d, iconCenter, p.success);
    }
    const ImVec2 textPos(pos.x + S(24.0f), pos.y);
    TextFit(d, g_fonts.regular, textPos, C(snap.state == PlayState::Failed ? p.danger : p.text), T(snap.messageKey.c_str()),
            maxWidth - S(24.0f));
    float y = pos.y + g_fonts.regular->FontSize + S(4.0f);
    if (snap.state == PlayState::Done || snap.state == PlayState::AlreadyLoaded) {
        TextFit(d, g_fonts.tiny, ImVec2(textPos.x, y), C(p.textDim), T("play.unload_hint"), maxWidth - S(24.0f));
    } else if (!snap.detail.empty()) {
        TextFit(d, g_fonts.tiny, ImVec2(textPos.x, y), C(p.textDim), snap.detail.c_str(), maxWidth - S(24.0f));
    }
}

void DrawHero(ImDrawList* d, ImVec2 min, ImVec2 max, const GameStatus& status) {
    const Palette& p = P();
    const float rounding = S(kRound + 4.0f);
    GradientRect(d, min, max, C(p.heroTop), C(p.heroBottom), rounding);

    // Formas del logo, grandes y tenues, moviéndose despacio a la derecha.
    const float t = static_cast<float>(ImGui::GetTime());
    const float h = max.y - min.y;
    d->PushClipRect(ImVec2(min.x + rounding * 0.4f, min.y + 1), ImVec2(max.x - rounding * 0.4f, max.y - 1), true);
    icons::Draw(d, icons::kLumaLogo,
                ImVec2(max.x - h * 0.55f + std::sin(t * 0.35f) * S(10.0f), min.y + h * 0.58f + std::cos(t * 0.3f) * S(8.0f)),
                h * 1.45f, C(p.heroShape, 1.6f));
    icons::Draw(d, icons::kLumaLogo,
                ImVec2(max.x - h * 1.45f + std::cos(t * 0.27f) * S(14.0f), min.y + h * 0.95f + std::sin(t * 0.33f) * S(6.0f)),
                h * 0.9f, C(p.heroShape));
    // Un brillo suave
    const ImVec2 glow(max.x - h * 0.75f, min.y + h * 0.35f);
    // (muchos círculos casi transparentes = un brillo sin "anillos")
    for (int i = 24; i >= 1; --i) {
        d->AddCircleFilled(glow, h * 0.022f * i, C(p.text, 0.0045f), 48);
    }
    d->PopClipRect();
    d->AddRect(min, max, C(p.border), rounding, 0, 1.0f);

    // Píldora arriba a la derecha: el estado de Luma
    const bool loaded = status.lumaLoaded;
    const char* pill = loaded ? T("home.luma_loaded") : T("home.ready");
    const ImVec2 ps = TextSize(g_fonts.tiny, pill);
    const ImVec2 pmax(max.x - S(20.0f), min.y + S(20.0f) + S(30.0f));
    const ImVec2 pmin(pmax.x - ps.x - S(40.0f), min.y + S(20.0f));
    d->AddRectFilled(pmin, pmax, C(p.background, 0.55f), S(15.0f));
    d->AddRect(pmin, pmax, C(p.border), S(15.0f));
    Dot(d, ImVec2(pmin.x + S(16.0f), (pmin.y + pmax.y) * 0.5f), loaded ? p.success : p.textDim);
    Text(d, g_fonts.tiny, ImVec2(pmin.x + S(28.0f), (pmin.y + pmax.y) * 0.5f - ps.y * 0.5f), C(p.text), pill);

    // Textos
    const float left = min.x + S(36.0f);
    float y = min.y + S(34.0f);
    Text(d, g_fonts.semibold, ImVec2(left, y), C(p.textDim), T("home.welcome"));
    y += g_fonts.semibold->FontSize + S(2.0f);
    TextFit(d, g_fonts.hero, ImVec2(left, y), C(p.text), PlayerName(), (max.x - min.x) * 0.55f);
    y += g_fonts.hero->FontSize + S(4.0f);
    Text(d, g_fonts.regular, ImVec2(left, y), C(p.textDim), T("home.subtitle"));

    // Botón JUGAR
    const PlaySnapshot snap = g_flow.Snapshot();
    const bool busy = g_flow.Busy();
    const ImVec2 bmin(left, max.y - S(36.0f) - S(58.0f) - S(34.0f));
    const ImVec2 bmax(bmin.x + S(210.0f), bmin.y + S(58.0f));
    float hover = 0.0f;
    if (Hit("##play", bmin, bmax, &hover, !busy)) {
        g_flow.Start();
    }
    const float press = ImGui::IsItemActive() && !busy ? 1.0f : 0.0f;
    const float lift = S(2.0f) * hover * (1.0f - press);
    const ImVec2 lmin(bmin.x, bmin.y - lift);
    const ImVec2 lmax(bmax.x, bmax.y - lift);
    // sombra suave
    d->AddRectFilled(ImVec2(lmin.x + S(2), lmin.y + S(6)), ImVec2(lmax.x - S(2), lmax.y + S(6)),
                     C(ImVec4(0, 0, 0, 1), 0.18f + 0.10f * hover), S(14.0f));
    const ImVec4 fill = busy ? Mix(p.accent, p.card, 0.35f) : Mix(p.accent, p.text, 0.0f);
    d->AddRectFilled(lmin, lmax, C(fill), S(14.0f));
    d->AddRectFilled(lmin, lmax, C(p.onAccent, 0.08f * hover), S(14.0f));
    const char* label = T("home.play");
    const ImVec2 ls = TextSize(g_fonts.title, label);
    const float contentW = S(22.0f) + S(14.0f) + ls.x;
    const float cx = (lmin.x + lmax.x) * 0.5f - contentW * 0.5f;
    const float cy = (lmin.y + lmax.y) * 0.5f;
    if (busy) {
        Spinner(d, ImVec2(cx + S(11.0f), cy), S(9.0f), C(p.onAccent));
    } else {
        icons::Draw(d, kIconPlay, ImVec2(cx + S(11.0f), cy), S(24.0f), C(p.onAccent));
    }
    Text(d, g_fonts.title, ImVec2(cx + S(36.0f), cy - ls.y * 0.5f), C(p.onAccent), label);

    // Estado debajo del botón
    DrawPlayStatus(d, ImVec2(left, bmax.y + S(12.0f)), (max.x - min.x) - S(72.0f), snap);
}

void SectionTitle(ImDrawList* d, ImVec2 pos, const char* text) {
    const Palette& p = P();
    d->AddRectFilled(ImVec2(pos.x, pos.y + S(3.0f)), ImVec2(pos.x + S(4.0f), pos.y + S(19.0f)), C(p.accent), S(2.0f));
    Text(d, g_fonts.semibold, ImVec2(pos.x + S(14.0f), pos.y), C(p.text), text);
}

void DrawQuickAccess(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("home.quick_access"));
    struct Tile {
        Page page;
        const char* icon;
        const char* title;
        const char* sub;
    };
    const Tile tiles[] = {
        {Page::Addons, kIconAddons, "nav.addons", "home.qa_addons"},
        {Page::Skins, kIconSkins, "nav.skins", "home.qa_skins"},
        {Page::Settings, icons::kSettings, "nav.settings", "home.qa_settings"},
    };
    const float top = min.y + S(56.0f);
    const float bottom = max.y - S(18.0f);
    const float gap = S(12.0f);
    const float tileW = ((max.x - min.x) - S(40.0f) - gap * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 tmin(min.x + S(20.0f) + i * (tileW + gap), top);
        const ImVec2 tmax(tmin.x + tileW, bottom);
        char id[16];
        std::snprintf(id, sizeof(id), "##qa%d", i);
        float hover = 0.0f;
        if (Hit(id, tmin, tmax, &hover)) {
            GoTo(tiles[i].page);
        }
        d->AddRectFilled(tmin, tmax, C(Mix(p.background, p.cardHover, hover), 0.9f), S(12.0f));
        d->AddRect(tmin, tmax, C(Mix(p.border, p.accent, hover * 0.35f)), S(12.0f));
        const float h = tmax.y - tmin.y;
        const float cx = (tmin.x + tmax.x) * 0.5f;
        const float iconY = tmin.y + h * 0.34f - S(3.0f) * hover;
        icons::Draw(d, tiles[i].icon, ImVec2(cx, iconY), S(28.0f), C(Mix(p.textDim, p.text, hover)), S(1.8f));
        const char* title = T(tiles[i].title);
        const ImVec2 ts = TextSize(g_fonts.semibold, title);
        Text(d, g_fonts.semibold, ImVec2(cx - ts.x * 0.5f, tmin.y + h * 0.56f), C(p.text), title);
        const char* sub = T(tiles[i].sub);
        const ImVec2 ss = TextSize(g_fonts.tiny, sub);
        if (ss.x < tileW - S(12.0f)) {
            Text(d, g_fonts.tiny, ImVec2(cx - ss.x * 0.5f, tmin.y + h * 0.56f + ts.y + S(4.0f)), C(p.textDim), sub);
        }
    }
}

void InfoRow(ImDrawList* d, ImVec2 pos, float width, const char* label, const char* value, const ImVec4* dot) {
    const Palette& p = P();
    Text(d, g_fonts.regular, pos, C(p.textDim), label);
    const ImVec2 vs = TextSize(g_fonts.semibold, value);
    const float vx = pos.x + width - vs.x;
    Text(d, g_fonts.semibold, ImVec2(vx, pos.y), C(p.text), value);
    if (dot != nullptr) {
        Dot(d, ImVec2(vx - S(14.0f), pos.y + vs.y * 0.5f), *dot);
    }
}

void DrawClientInfo(ImDrawList* d, ImVec2 min, ImVec2 max, const GameStatus& status) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("home.client_info"));
    const float x = min.x + S(22.0f);
    const float w = (max.x - min.x) - S(44.0f);
    const float rowH = std::min(S(36.0f), ((max.y - min.y) - S(70.0f)) / 4.0f);
    float y = min.y + S(60.0f);
    char version[32];
    std::snprintf(version, sizeof(version), "v%s", kVersion);
    InfoRow(d, ImVec2(x, y), w, T("home.luma_version"), version, nullptr);
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("home.game_version"),
            status.gameVersion.empty() ? T("home.not_installed") : status.gameVersion.c_str(), nullptr);
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("home.game"), status.running ? T("home.game_running") : T("home.game_closed"),
            status.running ? &p.success : &p.textDim);
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("home.luma"), status.lumaLoaded ? T("home.luma_loaded") : T("home.luma_unloaded"),
            status.lumaLoaded ? &p.success : &p.textDim);
}

void DrawBanner(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    const float rounding = S(kRound);
    GradientRect(d, min, max, C(p.heroBottom), C(p.heroTop), rounding);
    const float h = max.y - min.y;
    d->PushClipRect(ImVec2(min.x + rounding * 0.5f, min.y + 1), ImVec2(max.x - rounding * 0.5f, max.y - 1), true);
    const float t = static_cast<float>(ImGui::GetTime());
    for (int i = 0; i < 4; ++i) {
        const float fx = max.x - h * (0.9f + i * 1.25f) + std::sin(t * 0.25f + i) * S(6.0f);
        icons::Draw(d, icons::kLumaLogo, ImVec2(fx, min.y + h * (0.75f + 0.1f * (i % 2))), h * (1.6f - i * 0.18f),
                    C(p.heroShape));
    }
    d->PopClipRect();
    d->AddRect(min, max, C(p.border), rounding);
    const float cy = (min.y + max.y) * 0.5f;
    icons::Draw(d, icons::kLumaLogo, ImVec2(min.x + S(48.0f), cy), S(38.0f), C(p.text));
    const char* brand = "LUMA";
    const ImVec2 bs = TextSize(g_fonts.title, brand);
    Text(d, g_fonts.title, ImVec2(min.x + S(78.0f), cy - bs.y * 0.5f), C(p.text), brand);
    const float lineX = min.x + S(78.0f) + bs.x + S(22.0f);
    d->AddLine(ImVec2(lineX, cy - S(14.0f)), ImVec2(lineX, cy + S(14.0f)), C(p.border), 1.0f);
    const char* tagline = T("home.banner");
    const ImVec2 ts = TextSize(g_fonts.regular, tagline);
    Text(d, g_fonts.regular, ImVec2(lineX + S(22.0f), cy - ts.y * 0.5f), C(p.textDim), tagline);
}

void DrawHome(ImDrawList* d, ImVec2 min, ImVec2 max, const GameStatus& status) {
    const float width = max.x - min.x;
    const float height = max.y - min.y;
    const float gap = S(kGap);
    // Alturas: el cartel se adapta; la franja de la marca solo si hay lugar.
    const float bannerH = height > S(600.0f) ? S(86.0f) : 0.0f;
    const float heroH = std::clamp(height * 0.48f, S(270.0f), S(360.0f));
    const float rowH = height - heroH - gap - (bannerH > 0 ? bannerH + gap : 0.0f);
    DrawHero(d, min, ImVec2(max.x, min.y + heroH), status);
    const float rowY = min.y + heroH + gap;
    const float qaW = width * 0.58f;
    DrawQuickAccess(d, ImVec2(min.x, rowY), ImVec2(min.x + qaW, rowY + rowH));
    DrawClientInfo(d, ImVec2(min.x + qaW + gap, rowY), ImVec2(max.x, rowY + rowH), status);
    if (bannerH > 0.0f) {
        DrawBanner(d, ImVec2(min.x, max.y - bannerH), max);
    }
}

// Secciones que llegan en las próximas partes.
void DrawComingSoon(ImDrawList* d, ImVec2 min, ImVec2 max, const char* icon, const char* titleKey, const char* textKey) {
    const Palette& p = P();
    Card(d, min, max);
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f - S(30.0f));
    d->AddCircleFilled(c, S(42.0f), C(p.background));
    icons::Draw(d, icon, c, S(40.0f), C(p.textDim), S(1.8f));
    const char* title = T(titleKey);
    const ImVec2 ts = TextSize(g_fonts.title, title);
    Text(d, g_fonts.title, ImVec2(c.x - ts.x * 0.5f, c.y + S(58.0f)), C(p.text), title);
    const char* soon = T("soon.title");
    const ImVec2 ss = TextSize(g_fonts.semibold, soon);
    Text(d, g_fonts.semibold, ImVec2(c.x - ss.x * 0.5f, c.y + S(58.0f) + ts.y + S(8.0f)), C(p.textDim), soon);
    const char* text = T(textKey);
    const ImVec2 xs = TextSize(g_fonts.regular, text);
    Text(d, g_fonts.regular, ImVec2(c.x - xs.x * 0.5f, c.y + S(58.0f) + ts.y + ss.y + S(18.0f)), C(p.textDim), text);
}

void StatusThread() {
    while (!g_quit.load()) {
        GameStatus status = QueryGameStatus();
        {
            std::lock_guard lock(g_statusMutex);
            g_status = std::move(status);
        }
        for (int i = 0; i < 15 && !g_quit.load(); ++i) {
            Sleep(100);
        }
    }
}

}  // namespace

void AppInit(HWND hwnd, float scale) {
    g_hwnd = hwnd;
    g_scale = scale;
    LoadConfig();
    AppRebuildFonts();
    g_statusThread = std::thread(StatusThread);
}

void AppSetScale(float scale) {
    g_scale = scale;
    g_fontsDirty = true;
}

bool AppFontsDirty() {
    return g_fontsDirty;
}

void AppRebuildFonts() {
    g_fontsDirty = false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    const fs::path dir = fs::path(windowsDir) / L"Fonts";
    auto load = [&](const wchar_t* file, float size) -> ImFont* {
        std::error_code ec;
        const fs::path path = dir / file;
        if (!fs::exists(path, ec)) {
            return nullptr;
        }
        ImFontConfig config;
        config.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size * g_scale, &config);
    };
    g_fonts.regular = load(L"segoeui.ttf", 15.0f);
    if (g_fonts.regular == nullptr) {
        g_fonts.regular = io.Fonts->AddFontDefault();
    }
    g_fonts.tiny = load(L"segoeui.ttf", 13.0f);
    g_fonts.semibold = load(L"seguisb.ttf", 16.0f);
    g_fonts.title = load(L"segoeuib.ttf", 22.0f);
    g_fonts.hero = load(L"segoeuib.ttf", 42.0f);
    ImFont* fallback = g_fonts.regular;
    if (g_fonts.tiny == nullptr) g_fonts.tiny = fallback;
    if (g_fonts.semibold == nullptr) g_fonts.semibold = fallback;
    if (g_fonts.title == nullptr) g_fonts.title = g_fonts.semibold;
    if (g_fonts.hero == nullptr) g_fonts.hero = g_fonts.title;
    io.Fonts->Build();
}

void AppFrame() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Palette& p = P();

    GameStatus status;
    {
        std::lock_guard lock(g_statusMutex);
        status = g_status;
    }
    // Al terminar JUGAR con éxito: minimizar (un instante después, para que
    // se alcance a ver "Luma está funcionando").
    const PlayState state = g_flow.Snapshot().state;
    if (state != g_lastState) {
        if (state == PlayState::Done) {
            g_minimizeAt = ImGui::GetTime() + 1.5;
        }
        g_lastState = state;
    }
    if (g_minimizeAt > 0.0 && ImGui::GetTime() >= g_minimizeAt) {
        g_minimizeAt = 0.0;
        ShowWindow(g_hwnd, SW_MINIMIZE);
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##luma_launcher", nullptr, kFlags);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(ImVec2(0, 0), display, Col(p.background));

    g_fade = 1.0f;
    DrawSidebar(d, display);

    // Contenido (con un fundido al cambiar de sección)
    g_pageFade = std::min(1.0f, g_pageFade + Dt() * 5.0f);
    const float appear = g_pageFade * g_pageFade * (3.0f - 2.0f * g_pageFade);
    g_fade = appear;
    const float offset = (1.0f - appear) * S(10.0f);
    const ImVec2 cmin(S(kSidebarWidth) + S(kPad), S(kTitleBarHeight) + offset);
    const ImVec2 cmax(display.x - S(kPad), display.y - S(kPad) + offset);
    switch (g_page) {
        case Page::Home: DrawHome(d, cmin, cmax, status); break;
        case Page::Addons: DrawComingSoon(d, cmin, cmax, kIconAddons, "nav.addons", "soon.addons"); break;
        case Page::Skins: DrawComingSoon(d, cmin, cmax, kIconSkins, "nav.skins", "soon.skins"); break;
        case Page::Settings: DrawComingSoon(d, cmin, cmax, icons::kSettings, "nav.settings", "soon.settings"); break;
    }
    g_fade = 1.0f;
    DrawTitleBar(d, display);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void AppShutdown() {
    g_quit.store(true);
    if (g_statusThread.joinable()) {
        g_statusThread.join();
    }
}

bool AppIsCaption(int x, int y) {
    if (static_cast<float>(y) >= S(kTitleBarHeight)) {
        return false;
    }
    const ImVec2 p(static_cast<float>(x), static_cast<float>(y));
    for (int i = 0; i < g_noDragCount; ++i) {
        if (p.x >= g_noDrag[i].min.x && p.x < g_noDrag[i].max.x && p.y >= g_noDrag[i].min.y && p.y < g_noDrag[i].max.y) {
            return false;
        }
    }
    return true;
}

}  // namespace luma::launcher::gui
