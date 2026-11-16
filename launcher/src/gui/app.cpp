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
//    │  Add-ons │  ┌ Acceso rápido ─────────┐ ┌ Tus estadísticas ┐  │
//    │  Skins   │  └────────────────────────┘ └──────────────────┘  │
//    │  Ajustes │  ┌ Últimas noticias ──────┐ ┌ Info del cliente ┐  │
//    │ (perfil) │  └────────────────────────┘ └──────────────────┘  │
//    └──────────┴───────────────────────────────────────────────────┘
//
//  ANIMACIONES: cada elemento guarda un número de 0 a 1 ("qué tan encima
//  está el mouse") que avanza un poco cada frame; con eso se mezclan los
//  colores. Son cuentas mínimas: no afectan el rendimiento.
// =============================================================================
#include "gui/app.h"

#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>

#include <imgui.h>
#include <imgui_internal.h>  // ShadeVertsLinearColorGradientKeepAlpha (degradados)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "account.h"
#include "core/audio_viz.h"
#include "core/media.h"
#include "perf/timer.h"
#include "core/version.h"
#include "flow.h"
#include "gui/sky.h"
#include "gui/skin3d.h"
#include "gui/skins.h"
#include "gui/textures.h"
#include "gui/theme.h"
#include "i18n.h"
#include "render/icons.h"
#include "updater.h"

namespace luma::launcher::gui {

namespace {

namespace fs = std::filesystem;
namespace icons = render::icons;
using i18n::T;

// --- Íconos propios del launcher (cuadrícula de 24x24, como los del menú) -------
// (Casi todos de Lucide: ver client/render/icons.h.)
constexpr const char* kIconHome = icons::kHome;
constexpr const char* kIconAddons = icons::kBox;  // un bloque
constexpr const char* kIconSkins = icons::kCloset;
constexpr const char* kIconPlay = icons::kPlayFilled;
constexpr const char* kIconSun = icons::kSun;
constexpr const char* kIconMoon = icons::kMoon;
constexpr const char* kIconMinimize = icons::kMinus;
constexpr const char* kIconMaximize = icons::kSquare;
constexpr const char* kIconRestore = icons::kCopy;
constexpr const char* kIconAlert = icons::kAlert;
constexpr const char* kIconClock = icons::kClock;
constexpr const char* kIconSessions = icons::kAppWindow;
constexpr const char* kIconNews = icons::kNewspaper;
constexpr const char* kIconSliders = icons::kSliders;
constexpr const char* kIconCalendar = icons::kCalendar;
constexpr const char* kIconWindows = icons::kWindowsLogo;
constexpr const char* kIconStar = icons::kStarFilled;
constexpr const char* kIconCheck = icons::kCheck;
constexpr const char* kIconChevron = icons::kChevronRight;
constexpr const char* kIconFolder = icons::kFolder;
constexpr const char* kIconUser = icons::kProfiles;

// --- Medidas (a 100% de escala; se multiplican por g_scale) ---------------------
constexpr float kSidebarWidth = 232.0f;
constexpr float kTitleBarHeight = 48.0f;
constexpr float kPad = 24.0f;      // margen del contenido
constexpr float kGap = 18.0f;      // separación entre tarjetas
constexpr float kRound = 14.0f;    // esquinas de las tarjetas

// (Add-ons and Skins are kept in the code, but the music launcher shows
// Home, Lyrics, Spotify and Settings.)
enum class Page { Home = 0, Addons, Skins, Settings, Lyrics, Spotify };

// --- Estado ---------------------------------------------------------------------
HWND g_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
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
    ImFont* pixel = nullptr;     // la letra pixelada de Minecraft (de TU juego instalado), para el nombre sobre la skin
} g_fonts;
std::string g_pixelFontDir;      // de qué carpeta del juego se cargó (si cambia, se recarga)

// Zonas de la barra de título que NO sirven para arrastrar (los botones).
struct Rect {
    ImVec2 min, max;
};
Rect g_noDrag[6];
int g_noDragCount = 0;

PlayFlow g_flow;
PlayState g_lastState = PlayState::Idle;
double g_minimizeAt = 0.0;  // cuándo minimizar después de JUGAR (0 = no)

// Ajustes del launcher (launcher.json)
bool g_minimizeAfterPlay = true;
bool g_startWithWindows = false;  // la verdad está en el registro: se lee al abrir
bool g_autoUpdate = true;         // actualizarse solo al abrir
sky::Mode g_skyMode = sky::Mode::Auto;  // el cielo del cartel: según la hora o fijo
updater::Info g_upd;              // el estado del actualizador en este frame
bool g_closing = false;

// Lo que se consulta en un hilo aparte cada 1,5 s (listar procesos y leer
// archivos no es gratis: no lo hacemos en cada frame).
struct LocalData {
    GameStatus game;
    std::string profileName;      // del juego (profile.json, lo escribe Luma.dll)
    long long sessions = 0;       // stats.json
    long long playSeconds = 0;
    long long lastPlayed = 0;     // segundos desde 1970 (0 = nunca)
    // Tu skin (la guarda Luma.dll en skin.png). "skinKey" cambia cuando cambia el archivo.
    std::wstring skinPath;
    std::wstring skinKey;
    bool skinSlim = false;
    bool skinLegacy = false;
    std::string gameDir;          // la carpeta del juego (para su letra pixelada)
};
std::mutex g_dataMutex;
LocalData g_data;
std::thread g_statusThread;
std::atomic<bool> g_quit{false};

// La foto de Xbox como textura de Direct3D.
ID3D11ShaderResourceView* g_avatar = nullptr;
int g_avatarVersion = 0;

// --- Archivos y carpetas ----------------------------------------------------------
fs::path DataDir() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma";
    }
    CoTaskMemFree(raw);
    return path;
}

nlohmann::json ReadJson(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return nlohmann::json::object();
    }
    nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
    return json.is_object() ? json : nlohmann::json::object();
}

void LoadConfig() {
    const nlohmann::json json = ReadJson(DataDir() / L"launcher.json");
    SetTheme(json.value("theme", std::string("dark")) == "light" ? ThemeKind::Light : ThemeKind::Dark);
    i18n::SetLanguage(i18n::LanguageFromCode(json.value("language", std::string("en")).c_str()));
    g_minimizeAfterPlay = json.value("minimizeAfterPlay", true);
    g_autoUpdate = json.value("autoUpdate", true);
    g_skyMode = static_cast<sky::Mode>(std::clamp(json.value("sky", 0), 0, 3));
}

void SaveConfig() {
    const fs::path dir = DataDir();
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    nlohmann::json json;
    json["theme"] = CurrentTheme() == ThemeKind::Light ? "light" : "dark";
    json["language"] = i18n::LanguageCode(i18n::CurrentLanguage());
    json["minimizeAfterPlay"] = g_minimizeAfterPlay;
    json["autoUpdate"] = g_autoUpdate;
    json["sky"] = static_cast<int>(g_skyMode);
    std::ofstream(dir / L"launcher.json") << json.dump(2);
}

void OpenFolder(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- "Abrir con Windows" ---------------------------------------------------------------
// Es la clave "Run" del registro de TU usuario (no de todo el equipo): lo que
// está ahí se abre al iniciar sesión en Windows. Solo se toca si activas la
// opción, y se borra al desactivarla.
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"LumaLauncher";

std::wstring StartupCommand() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return L"\"" + std::wstring(exe, length) + L"\" --minimized";
}

bool ReadStartup() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

void WriteStartup(bool enabled) {
    if (enabled) {
        const std::wstring command = StartupCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
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

// Texto en varias líneas (corta por palabras). Devuelve la altura usada.
float TextWrap(ImDrawList* d, ImFont* font, ImVec2 pos, ImU32 color, const char* text, float width) {
    d->AddText(font, font->FontSize, pos, color, text, nullptr, width);
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, width, text).y;
}

// 0..1: how much the window is "glass" over the album's light (Home, with music).
float g_glass = 0.0f;

void Card(ImDrawList* d, ImVec2 min, ImVec2 max, float hover = 0.0f) {
    const Palette& p = P();
    // Over the album's light the cards are glass: see-through, with a fine
    // bright edge on top (like light catching the edge of a pane).
    const float alpha = 1.0f - 0.52f * g_glass;
    d->AddRectFilled(min, max, C(Mix(p.card, p.cardHover, hover), alpha), S(kRound));
    d->AddRect(min, max, C(Mix(p.border, ImVec4(1, 1, 1, 0.10f), g_glass)), S(kRound), 0, 1.0f);
    if (g_glass > 0.01f) {
        d->AddLine(ImVec2(min.x + S(kRound), min.y + 1.0f), ImVec2(max.x - S(kRound), min.y + 1.0f), C(ImVec4(1, 1, 1, 1), 0.10f * g_glass),
                   1.0f);
    }
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

// Rueda girando (mientras algo trabaja).
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

void SectionTitle(ImDrawList* d, ImVec2 pos, const char* text) {
    const Palette& p = P();
    d->AddRectFilled(ImVec2(pos.x, pos.y + S(3.0f)), ImVec2(pos.x + S(4.0f), pos.y + S(19.0f)), C(p.accent), S(2.0f));
    Text(d, g_fonts.semibold, ImVec2(pos.x + S(14.0f), pos.y), C(p.text), text);
}

// --- Controles de Ajustes ----------------------------------------------------------
// Botón con texto. "primary" = relleno con el acento. Devuelve si se hizo clic.
bool Button(const char* id, ImVec2 pos, const char* label, bool primary, float* width = nullptr, bool enabled = true) {
    const Palette& p = P();
    const ImVec2 ts = TextSize(g_fonts.semibold, label);
    const ImVec2 size(ts.x + S(32.0f), S(38.0f));
    if (width != nullptr) *width = size.x;
    float hover = 0.0f;
    const bool clicked = Hit(id, pos, ImVec2(pos.x + size.x, pos.y + size.y), &hover, enabled);
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float a = enabled ? 1.0f : 0.45f;
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (primary) {
        d->AddRectFilled(pos, max, C(p.accent, a), S(10.0f));
        d->AddRectFilled(pos, max, C(p.onAccent, 0.08f * hover), S(10.0f));
    } else {
        d->AddRectFilled(pos, max, C(Mix(p.background, p.cardHover, hover), a), S(10.0f));
        d->AddRect(pos, max, C(p.border), S(10.0f));
    }
    Text(d, g_fonts.semibold, ImVec2(pos.x + S(16.0f), pos.y + (size.y - ts.y) * 0.5f), C(primary ? p.onAccent : p.text, a),
         label);
    return clicked;
}

// Ancho que ocupa un Segmented con estas opciones.
float SegmentedWidth(const char* const* labels, int count) {
    float total = S(6.0f);
    for (int i = 0; i < count; ++i) {
        total += TextSize(g_fonts.semibold, labels[i]).x + S(28.0f);
    }
    return total;
}

// Varias opciones en fila, una elegida (como los botones de un control remoto).
bool Segmented(const char* id, ImVec2 pos, const char* const* labels, int count, int* index, float* outWidth = nullptr) {
    const Palette& p = P();
    ImDrawList* d = ImGui::GetWindowDrawList();
    float widths[4]{};
    float total = S(6.0f);
    for (int i = 0; i < count; ++i) {
        widths[i] = TextSize(g_fonts.semibold, labels[i]).x + S(28.0f);
        total += widths[i];
    }
    const float h = S(38.0f);
    if (outWidth != nullptr) *outWidth = total;
    d->AddRectFilled(pos, ImVec2(pos.x + total, pos.y + h), C(p.background), S(10.0f));
    d->AddRect(pos, ImVec2(pos.x + total, pos.y + h), C(p.border), S(10.0f));
    bool changed = false;
    float x = pos.x + S(3.0f);
    for (int i = 0; i < count; ++i) {
        char itemId[64];
        std::snprintf(itemId, sizeof(itemId), "%s%d", id, i);
        const ImVec2 min(x, pos.y + S(3.0f));
        const ImVec2 max(x + widths[i], pos.y + h - S(3.0f));
        float hover = 0.0f;
        if (Hit(itemId, min, max, &hover) && *index != i) {
            *index = i;
            changed = true;
        }
        const float on = Anim(ImGui::GetID(itemId) + 7, *index == i, 12.0f);
        if (on > 0.0f) d->AddRectFilled(min, max, C(p.accent, on), S(8.0f));
        if (hover > 0.0f && on < 1.0f) d->AddRectFilled(min, max, C(p.text, 0.05f * hover * (1.0f - on)), S(8.0f));
        const ImVec2 ts = TextSize(g_fonts.semibold, labels[i]);
        Text(d, g_fonts.semibold, ImVec2(min.x + (widths[i] - ts.x) * 0.5f, min.y + (max.y - min.y - ts.y) * 0.5f),
             C(Mix(p.textDim, p.onAccent, on)), labels[i]);
        x += widths[i];
    }
    return changed;
}

// Interruptor de encendido/apagado.
bool Toggle(const char* id, ImVec2 pos, bool* value) {
    const Palette& p = P();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 size(S(46.0f), S(26.0f));
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    float hover = 0.0f;
    const bool clicked = Hit(id, pos, max, &hover);
    if (clicked) {
        *value = !*value;
    }
    const float on = Anim(ImGui::GetID(id) + 3, *value, 10.0f);
    d->AddRectFilled(pos, max, C(Mix(Mix(p.background, p.cardHover, hover), p.accent, on)), size.y * 0.5f);
    d->AddRect(pos, max, C(p.border), size.y * 0.5f);
    const float r = size.y * 0.5f - S(4.0f);
    const float cx = pos.x + size.y * 0.5f + (size.x - size.y) * on;
    d->AddCircleFilled(ImVec2(cx, pos.y + size.y * 0.5f), r, C(Mix(p.textDim, p.onAccent, on)));
    return clicked;
}

// --- Nombre y foto del jugador -----------------------------------------------------
std::string PlayerName(const account::Info& acc, const LocalData& data) {
    if (acc.state == account::State::SignedIn && !acc.gamertag.empty()) return acc.gamertag;
    if (!data.profileName.empty()) return data.profileName;
    return T("nav.guest");
}

// Sube la foto de Xbox a la GPU cuando cambia (una vez por foto, no por frame).
void UpdateAvatar(const account::Info& acc) {
    if (acc.pictureVersion == g_avatarVersion) {
        return;
    }
    g_avatarVersion = acc.pictureVersion;
    if (g_avatar != nullptr) {
        g_avatar->Release();
        g_avatar = nullptr;
    }
    if (acc.pictureVersion <= 0 || g_device == nullptr) {
        return;
    }
    const account::Info full = account::Get(true);
    if (full.picture.empty() || full.pictureWidth <= 0) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(full.pictureWidth);
    desc.Height = static_cast<UINT>(full.pictureHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{full.picture.data(), static_cast<UINT>(full.pictureWidth * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture))) {
        g_device->CreateShaderResourceView(texture, nullptr, &g_avatar);
        texture->Release();
    }
}

// Círculo con la foto (o la inicial del nombre si no hay foto).
void Avatar(ImDrawList* d, ImVec2 center, float radius, const std::string& name) {
    const Palette& p = P();
    if (g_avatar != nullptr) {
        d->AddImageRounded(reinterpret_cast<ImTextureID>(g_avatar), ImVec2(center.x - radius, center.y - radius),
                           ImVec2(center.x + radius, center.y + radius), ImVec2(0, 0), ImVec2(1, 1), C(ImVec4(1, 1, 1, 1)),
                           radius);
        d->AddCircle(center, radius, C(p.border), 0, 1.0f);
        return;
    }
    d->AddCircleFilled(center, radius, C(p.card));
    d->AddCircle(center, radius, C(p.border), 0, 1.0f);
    char initial[5] = {name.empty() ? 'P' : name[0], 0};
    ImFont* font = radius > S(24.0f) ? g_fonts.title : g_fonts.semibold;
    const ImVec2 is = TextSize(font, initial);
    Text(d, font, ImVec2(center.x - is.x * 0.5f, center.y - is.y * 0.5f), C(p.text), initial);
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
                // PostMessage (no ShowWindow): el cambio de tamaño pasa DESPUÉS de
                // este frame. Si pasara ahora, Windows nos pediría dibujar otro
                // frame en medio de este, y eso crasheaba al maximizar.
                case 1: PostMessageW(g_hwnd, WM_SYSCOMMAND, maximized ? SC_RESTORE : SC_MAXIMIZE, 0); break;
                case 2: PostMessageW(g_hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
                case 3:
                    SetTheme(CurrentTheme() == ThemeKind::Dark ? ThemeKind::Light : ThemeKind::Dark);
                    SaveConfig();
                    break;
                default: break;
            }
        }
        x -= w + S(2.0f);
        if (b.action == 2) {
            // separador antes del botón de tema
            d->AddLine(ImVec2(x + w - S(4.0f), y + S(9.0f)), ImVec2(x + w - S(4.0f), y + h - S(9.0f)), Col(p.border), 1.0f);
            x -= S(8.0f);
        }
    }
#ifndef NDEBUG
    // Compilación de depuración: que se note (inyecta la Luma.dll de Debug,
    // que abre una consola y carga más lento).
    const char* badge = "DEBUG";
    const ImVec2 bs = TextSize(g_fonts.tiny, badge);
    const ImVec2 bmax(x + w - S(8.0f), y + h - S(5.0f));
    const ImVec2 bmin(bmax.x - bs.x - S(20.0f), y + S(5.0f));
    d->AddRectFilled(bmin, bmax, Col(p.warning, 0.18f), S(8.0f));
    d->AddRect(bmin, bmax, Col(p.warning, 0.6f), S(8.0f));
    Text(d, g_fonts.tiny, ImVec2(bmin.x + S(10.0f), (bmin.y + bmax.y - bs.y) * 0.5f), Col(p.warning), badge);
#endif
}

// --- Barra lateral --------------------------------------------------------------------
// --- Decoración y secciones nuevas -------------------------------------------------------
// Abrir una página web en el navegador (solo direcciones de Luma, fijas).
void OpenUrl(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// Un brillo suave: el color arriba con "alphaTop" que se desvanece hasta 0 abajo.
void Glow(ImDrawList* d, ImVec2 min, ImVec2 max, const ImVec4& color, float alphaTop, float rounding) {
    const int start = d->VtxBuffer.Size;
    d->AddRectFilled(min, max, C(color, alphaTop), rounding, ImDrawFlags_RoundCornersTop);
    const float h = std::max(1.0f, max.y - min.y);
    for (int i = start; i < d->VtxBuffer.Size; ++i) {
        ImDrawVert& v = d->VtxBuffer[i];
        const float t = std::clamp((v.pos.y - min.y) / h, 0.0f, 1.0f);
        const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(a) * (1.0f - t)) << IM_COL32_A_SHIFT);
    }
}

// Un "chip" con el nombre de una tecla (L, Fin, Tab...).
float KeyCap(ImDrawList* d, ImVec2 pos, const char* key) {
    const Palette& p = P();
    const ImVec2 ts = TextSize(g_fonts.semibold, key);
    const float w = std::max(S(34.0f), ts.x + S(18.0f));
    const float h = S(32.0f);
    d->AddRectFilled(ImVec2(pos.x, pos.y + S(2.0f)), ImVec2(pos.x + w, pos.y + h + S(2.0f)), C(p.border), S(8.0f));
    d->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), C(p.background), S(8.0f));
    d->AddRect(pos, ImVec2(pos.x + w, pos.y + h), C(p.border), S(8.0f));
    Text(d, g_fonts.semibold, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), C(p.text), key);
    return w;
}

// Estrellitas de píxel que titilan alrededor del logo de la barra lateral.
void DrawSidebarStars(ImDrawList* d, float width) {
    const Palette& p = P();
    struct Star {
        float x, y, size, phase;
    };
    static const Star kStars[] = {
        {0.18f, 28.0f, 3.0f, 0.0f}, {0.80f, 40.0f, 2.0f, 1.7f}, {0.30f, 104.0f, 2.0f, 3.1f},
        {0.86f, 96.0f, 3.0f, 4.4f}, {0.08f, 70.0f, 2.0f, 2.2f}, {0.68f, 18.0f, 2.0f, 5.3f},
    };
    const float t = static_cast<float>(ImGui::GetTime());
    for (const Star& s : kStars) {
        const float a = 0.12f + 0.22f * (0.5f + 0.5f * std::sin(t * 1.3f + s.phase));
        const float px = std::floor(width * s.x);
        const float py = std::floor(S(s.y));
        const float sz = std::floor(S(s.size));
        d->AddRectFilled(ImVec2(px, py), ImVec2(px + sz, py + sz), Col(p.text, a));
    }
}

// Tarjeta de "Consejo" en la barra lateral: cambia sola cada 9 segundos.
void DrawSidebarTip(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    static const char* const kTips[] = {"tip.menu", "tip.tab", "tip.unload", "tip.skins", "tip.view", "tip.zoom", "tip.language"};
    constexpr int kCount = static_cast<int>(sizeof(kTips) / sizeof(kTips[0]));
    constexpr double kEvery = 9.0;
    const double t = ImGui::GetTime();
    const int index = static_cast<int>(t / kEvery) % kCount;
    const float local = static_cast<float>(std::fmod(t, kEvery));
    const float fade = std::clamp(std::min(local / 0.5f, (static_cast<float>(kEvery) - local) / 0.5f), 0.0f, 1.0f);
    d->AddRectFilled(min, max, Col(p.card), S(12.0f));
    d->AddRect(min, max, Col(p.border), S(12.0f));
    // un brillo suave arriba
    Glow(d, min, ImVec2(max.x, min.y + (max.y - min.y) * 0.6f), p.text, 0.05f, S(12.0f));
    const ImVec2 ic(min.x + S(22.0f), min.y + S(22.0f));
    icons::Draw(d, kIconStar, ic, S(13.0f), Col(p.text, 0.85f));
    Text(d, g_fonts.tiny, ImVec2(ic.x + S(14.0f), ic.y - g_fonts.tiny->FontSize * 0.5f), Col(p.textDim), T("tip.title"));
    const float textW = max.x - min.x - S(28.0f);
    d->AddText(g_fonts.regular, g_fonts.regular->FontSize, ImVec2(min.x + S(14.0f), min.y + S(40.0f)), Col(p.text, fade),
               T(kTips[index]), nullptr, textW);
    // puntitos: cuál consejo es
    const float dotsW = kCount * S(9.0f);
    float dx = min.x + (max.x - min.x - dotsW) * 0.5f + S(3.0f);
    for (int i = 0; i < kCount; ++i) {
        d->AddCircleFilled(ImVec2(dx, max.y - S(12.0f)), S(2.2f), Col(p.text, i == index ? 0.8f : 0.18f));
        dx += S(9.0f);
    }
}

// --- Add-ons: los paquetes instalados en Minecraft ---------------------------------------
struct InstalledPack {
    std::string name;
    std::string version;
    int kind = 0;  // 0 = recursos, 1 = comportamiento, 2 = skins
};
std::vector<InstalledPack> g_packs;
int g_packCount[3]{};
bool g_packsScanned = false;
float g_packScroll = 0.0f;

fs::path MojangDir() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Minecraft Bedrock" / L"Users" / L"Shared" / L"games" / L"com.mojang";
    }
    CoTaskMemFree(raw);
    return path;
}

// Lee las carpetas de paquetes (solo leer: Luma no toca tus paquetes).
void ScanPacks() {
    g_packsScanned = true;
    g_packs.clear();
    for (int& c : g_packCount) c = 0;
    const fs::path base = MojangDir();
    const wchar_t* folders[3] = {L"resource_packs", L"behavior_packs", L"skin_packs"};
    std::error_code ec;
    for (int kind = 0; kind < 3; ++kind) {
        const fs::path dir = base / folders[kind];
        if (!fs::is_directory(dir, ec)) continue;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_directory(ec)) continue;
            const nlohmann::json manifest = ReadJson(e.path() / L"manifest.json");
            InstalledPack pack;
            pack.kind = kind;
            if (manifest.is_object() && manifest.contains("header") && manifest["header"].is_object()) {
                const nlohmann::json& h = manifest["header"];
                pack.name = h.value("name", std::string());
                if (h.contains("version") && h["version"].is_array() && h["version"].size() == 3) {
                    char v[32];
                    std::snprintf(v, sizeof(v), "%d.%d.%d", h["version"][0].get<int>(), h["version"][1].get<int>(),
                                  h["version"][2].get<int>());
                    pack.version = v;
                }
            }
            // Si el nombre es una clave de traducción ("pack.name") o está vacío, el de la carpeta.
            if (pack.name.empty() || pack.name.rfind("pack.", 0) == 0) {
                const std::u8string folder = e.path().filename().u8string();
                pack.name.assign(folder.begin(), folder.end());
            }
            g_packs.push_back(std::move(pack));
            ++g_packCount[kind];
            if (g_packs.size() >= 500) break;
        }
    }
    std::sort(g_packs.begin(), g_packs.end(), [](const InstalledPack& a, const InstalledPack& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });
}

const char* PackIcon(int kind) {
    return kind == 0 ? kIconAddons : kind == 1 ? kIconSliders : kIconSkins;
}

void DrawAddonsPage(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    if (!g_packsScanned) ScanPacks();
    // Encabezado
    Text(d, g_fonts.title, ImVec2(min.x + S(4.0f), min.y + S(2.0f)), C(p.text), T("nav.addons"));
    Text(d, g_fonts.tiny, ImVec2(min.x + S(4.0f), min.y + S(6.0f) + g_fonts.title->FontSize), C(p.textDim), T("addons.subtitle"));
    const char* openLabel = T("addons.open");
    const float openW = TextSize(g_fonts.semibold, openLabel).x + S(32.0f);
    if (Button("##addons_open", ImVec2(max.x - openW, min.y + S(4.0f)), openLabel, true)) {
        OpenFolder(MojangDir());
    }
    const char* refreshLabel = T("addons.refresh");
    const float refreshW = TextSize(g_fonts.semibold, refreshLabel).x + S(32.0f);
    if (Button("##addons_refresh", ImVec2(max.x - openW - S(10.0f) - refreshW, min.y + S(4.0f)), refreshLabel, false)) {
        ScanPacks();
    }

    // Tres cifras: cuántos paquetes de cada tipo.
    const float gap = S(kGap);
    const float top = min.y + S(66.0f);
    const float tileH = S(92.0f);
    const float tileW = (max.x - min.x - gap * 2.0f) / 3.0f;
    const char* labels[3] = {T("addons.resource"), T("addons.behavior"), T("addons.skin")};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 tmin(min.x + i * (tileW + gap), top);
        const ImVec2 tmax(tmin.x + tileW, top + tileH);
        Card(d, tmin, tmax);
        Glow(d, tmin, ImVec2(tmax.x, tmin.y + tileH * 0.7f), p.text, 0.045f, S(kRound));
        const ImVec2 ic(tmin.x + S(46.0f), tmin.y + tileH * 0.5f);
        d->AddCircleFilled(ic, S(24.0f), C(p.background));
        icons::Draw(d, PackIcon(i), ic, S(22.0f), C(p.text), S(1.7f));
        char number[16];
        std::snprintf(number, sizeof(number), "%d", g_packCount[i]);
        Text(d, g_fonts.title, ImVec2(ic.x + S(40.0f), ic.y - g_fonts.title->FontSize * 0.82f), C(p.text), number);
        Text(d, g_fonts.tiny, ImVec2(ic.x + S(40.0f), ic.y + S(6.0f)), C(p.textDim), labels[i]);
    }

    // Abajo: la lista (izquierda) y lo que viene (derecha).
    const float listTop = top + tileH + gap;
    const float sideW = std::clamp((max.x - min.x) * 0.3f, S(260.0f), S(360.0f));
    const ImVec2 lmin(min.x, listTop);
    const ImVec2 lmax(max.x - sideW - gap, max.y);
    Card(d, lmin, lmax);
    SectionTitle(d, ImVec2(lmin.x + S(20.0f), lmin.y + S(18.0f)), T("addons.installed"));
    const ImVec2 areaMin(lmin.x + S(12.0f), lmin.y + S(54.0f));
    const ImVec2 areaMax(lmax.x - S(12.0f), lmax.y - S(10.0f));
    const float rowH = S(48.0f);
    if (g_packs.empty()) {
        const char* empty = T("addons.empty");
        const ImVec2 es = TextSize(g_fonts.regular, empty);
        Text(d, g_fonts.regular, ImVec2((areaMin.x + areaMax.x - es.x) * 0.5f, (areaMin.y + areaMax.y - es.y) * 0.5f), C(p.textDim),
             empty);
    } else {
        // Scroll con la rueda, suave.
        const float content = g_packs.size() * rowH;
        const float maxScroll = std::max(0.0f, content - (areaMax.y - areaMin.y));
        static float target = 0.0f;
        if (ImGui::IsMouseHoveringRect(areaMin, areaMax)) target -= ImGui::GetIO().MouseWheel * rowH * 2.0f;
        target = std::clamp(target, 0.0f, maxScroll);
        g_packScroll += (target - g_packScroll) * std::min(1.0f, Dt() * 14.0f);
        d->PushClipRect(areaMin, areaMax, true);
        const char* kinds[3] = {T("addons.kind_resource"), T("addons.kind_behavior"), T("addons.kind_skin")};
        for (std::size_t i = 0; i < g_packs.size(); ++i) {
            const float y = areaMin.y + i * rowH - g_packScroll;
            if (y + rowH < areaMin.y || y > areaMax.y) continue;
            const InstalledPack& pk = g_packs[i];
            if (i % 2 == 0) d->AddRectFilled(ImVec2(areaMin.x, y), ImVec2(areaMax.x, y + rowH - S(4.0f)), C(p.background, 0.55f), S(10.0f));
            const ImVec2 ic(areaMin.x + S(24.0f), y + (rowH - S(4.0f)) * 0.5f);
            icons::Draw(d, PackIcon(pk.kind), ic, S(18.0f), C(p.textDim), S(1.6f));
            // tipo (chip) y versión a la derecha
            const ImVec2 ks = TextSize(g_fonts.tiny, kinds[pk.kind]);
            const float chipX = areaMax.x - S(14.0f) - ks.x - S(16.0f) - (pk.version.empty() ? 0.0f : S(70.0f));
            d->AddRectFilled(ImVec2(chipX, ic.y - S(11.0f)), ImVec2(chipX + ks.x + S(16.0f), ic.y + S(11.0f)), C(p.cardHover), S(11.0f));
            Text(d, g_fonts.tiny, ImVec2(chipX + S(8.0f), ic.y - ks.y * 0.5f), C(p.textDim), kinds[pk.kind]);
            if (!pk.version.empty()) {
                const ImVec2 vs = TextSize(g_fonts.tiny, pk.version.c_str());
                Text(d, g_fonts.tiny, ImVec2(areaMax.x - S(14.0f) - vs.x, ic.y - vs.y * 0.5f), C(p.textDim), pk.version.c_str());
            }
            TextFit(d, g_fonts.semibold, ImVec2(ic.x + S(22.0f), ic.y - g_fonts.semibold->FontSize * 0.5f), C(p.text), pk.name.c_str(),
                    chipX - ic.x - S(34.0f));
        }
        d->PopClipRect();
    }

    // Lo que viene en Add-ons.
    const ImVec2 smin(max.x - sideW, listTop);
    Card(d, smin, max);
    Glow(d, smin, ImVec2(max.x, smin.y + S(140.0f)), p.text, 0.05f, S(kRound));
    SectionTitle(d, ImVec2(smin.x + S(20.0f), smin.y + S(18.0f)), T("soon.title"));
    // el logo de Luma, grande y tenue, al pie de la tarjeta
    icons::Draw(d, icons::kLumaLogo, ImVec2((smin.x + max.x) * 0.5f, max.y - S(90.0f)), S(110.0f), C(p.text, 0.05f));
    const char* soon[] = {"addons.soon1", "addons.soon2", "addons.soon3", "addons.soon4"};
    float y = smin.y + S(62.0f);
    for (const char* key : soon) {
        d->AddCircleFilled(ImVec2(smin.x + S(30.0f), y + S(9.0f)), S(9.0f), C(p.background));
        icons::Draw(d, kIconCheck, ImVec2(smin.x + S(30.0f), y + S(9.0f)), S(11.0f), C(p.textDim), S(1.8f));
        y += TextWrap(d, g_fonts.regular, ImVec2(smin.x + S(48.0f), y), C(p.text), T(key), max.x - smin.x - S(66.0f)) + S(16.0f);
    }
}

// --- Skins: cómo ponerla en el juego -------------------------------------------------------
void DrawSkinGuide(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(16.0f)), T("skins.guide_title"));
    const char* steps[3] = {T("skins.guide1"), T("skins.guide2"), T("skins.guide3")};
    const float stepW = (max.x - min.x - S(40.0f)) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float x = min.x + S(20.0f) + i * stepW;
        const ImVec2 c(x + S(15.0f), min.y + S(68.0f));
        d->AddCircleFilled(c, S(15.0f), C(p.accent));
        char n[4];
        std::snprintf(n, sizeof(n), "%d", i + 1);
        const ImVec2 ns = TextSize(g_fonts.semibold, n);
        Text(d, g_fonts.semibold, ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.5f), C(p.onAccent), n);
        if (i < 2) d->AddLine(ImVec2(c.x + S(22.0f), c.y), ImVec2(x + stepW - S(8.0f), c.y), C(p.border), S(1.5f));
        TextWrap(d, g_fonts.tiny, ImVec2(x, c.y + S(24.0f)), C(p.textDim), steps[i], stepW - S(16.0f));
    }
}

// --- Ajustes: atajos y enlaces ------------------------------------------------------------
void DrawSettingsShortcuts(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("settings.shortcuts"));
    struct Shortcut {
        const char* key;
        const char* text;
    };
    const Shortcut items[] = {
        {"L", "shortcut.menu"}, {T("shortcut.end_key"), "shortcut.unload"}, {"Tab", "shortcut.tab"}, {"C", "shortcut.zoom"},
    };
    const float colW = (max.x - min.x - S(44.0f)) * 0.5f;
    for (int i = 0; i < 4; ++i) {
        const float x = min.x + S(22.0f) + (i % 2) * colW;
        const float y = min.y + S(52.0f) + (i / 2) * S(40.0f);
        const float kw = KeyCap(d, ImVec2(x, y), items[i].key);
        TextFit(d, g_fonts.regular, ImVec2(x + kw + S(12.0f), y + (S(32.0f) - g_fonts.regular->FontSize) * 0.5f), C(p.text),
                T(items[i].text), colW - kw - S(24.0f));
    }
}

void DrawSettingsLinks(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("settings.links"));
    float x = min.x + S(22.0f);
    float y = min.y + S(52.0f);
    float w = 0.0f;
    if (Button("##link_github", ImVec2(x, y), "GitHub", false, &w)) OpenUrl(L"https://github.com/IAOcd/Luma");
    x += w + S(10.0f);
    const char* issues = T("settings.report");
    if (x + TextSize(g_fonts.semibold, issues).x + S(32.0f) > max.x - S(22.0f)) {
        x = min.x + S(22.0f);
        y += S(46.0f);
    }
    if (Button("##link_issues", ImVec2(x, y), issues, false, &w)) OpenUrl(L"https://github.com/IAOcd/Luma/issues");
    TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(22.0f), y + S(50.0f)), C(p.textDim), T("settings.license"),
             max.x - min.x - S(44.0f));
}

void DrawSidebar(ImDrawList* d, ImVec2 display, const account::Info& acc, const LocalData& data) {
    const Palette& p = P();
    const float width = S(kSidebarWidth);
    d->AddRectFilled(ImVec2(0, 0), ImVec2(width, display.y), Col(p.sidebar, 1.0f - 0.58f * g_glass));
    d->AddLine(ImVec2(width, 0), ImVec2(width, display.y), Col(p.border));

    DrawSidebarStars(d, width);
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
        {Page::Lyrics, icons::kList, "nav.lyrics"},
        {Page::Spotify, icons::kMusic2, "nav.spotify"},
        {Page::Settings, kIconSliders, "nav.settings"},
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
            for (int g = 3; g >= 1; --g) {
                const float e = S(3.0f) * static_cast<float>(g);
                d->AddRectFilled(ImVec2(min.x - e, min.y - e), ImVec2(max.x + e, max.y + e), Col(p.accent, 0.07f * on), S(11.0f) + e);
            }
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

    // Perfil (abajo): clic = ir a Ajustes (donde está la cuenta)
    const float py = display.y - S(78.0f);
    // Un consejo arriba del perfil (si hay lugar).
    if (py - S(150.0f) > y + S(16.0f)) {
        DrawSidebarTip(d, ImVec2(S(14.0f), py - S(146.0f)), ImVec2(width - S(14.0f), py - S(16.0f)));
    }
    d->AddLine(ImVec2(S(18.0f), py), ImVec2(width - S(18.0f), py), Col(p.border));
    float hover = 0.0f;
    if (Hit("##profile", ImVec2(S(10.0f), py + S(8.0f)), ImVec2(width - S(10.0f), display.y - S(8.0f)), &hover)) {
        GoTo(Page::Settings);
    }
    if (hover > 0.0f) {
        d->AddRectFilled(ImVec2(S(10.0f), py + S(8.0f)), ImVec2(width - S(10.0f), display.y - S(8.0f)),
                         Col(p.text, 0.05f * hover), S(11.0f));
    }
    const std::string name = PlayerName(acc, data);
    const ImVec2 avatar(S(40.0f), py + S(39.0f));
    const float savedFade = g_fade;
    g_fade = 1.0f;
    Avatar(d, avatar, S(18.0f), name);
    g_fade = savedFade;
    TextFit(d, g_fonts.semibold, ImVec2(S(68.0f), avatar.y - S(19.0f)), Col(p.text), name.c_str(), width - S(84.0f));
    const bool signedIn = acc.state == account::State::SignedIn;
    if (signedIn) {
        Dot(d, ImVec2(S(73.0f), avatar.y + S(10.0f)), p.success);
    }
    TextFit(d, g_fonts.tiny, ImVec2(S(signedIn ? 84.0f : 68.0f), avatar.y + S(2.0f)), Col(p.textDim),
            signedIn ? T("account.online") : T("nav.not_signed_in"), width - S(110.0f));
    icons::Draw(d, kIconChevron, ImVec2(width - S(24.0f), avatar.y), S(14.0f), Col(Mix(p.textDim, p.text, hover)), S(1.8f));
}

// --- Inicio -----------------------------------------------------------------------------
void DrawPlayStatus(ImDrawList* d, ImVec2 pos, float maxWidth, const PlaySnapshot& snap) {
    const Palette& p = P();
    if (snap.state == PlayState::Idle) {
        return;
    }
    const bool busy = snap.state != PlayState::Done && snap.state != PlayState::AlreadyLoaded &&
                      snap.state != PlayState::Failed && snap.state != PlayState::Unsupported;
    const ImVec2 iconCenter(pos.x + S(8.0f), pos.y + g_fonts.regular->FontSize * 0.5f);
    if (snap.state == PlayState::Unsupported) {
        icons::Draw(d, kIconAlert, iconCenter, S(17.0f), C(p.warning), S(1.6f));
        char text[256];
        std::snprintf(text, sizeof(text), T("play.unsupported"), snap.detail.c_str());
        TextFit(d, g_fonts.regular, ImVec2(pos.x + S(24.0f), pos.y), C(p.warning), text, maxWidth - S(24.0f));
        return;
    }
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

// Texto con una sombra suave debajo (para que se lea sobre el cielo).
void ShadowText(ImDrawList* d, ImFont* font, ImVec2 pos, ImU32 color, const char* text, float width = 0.0f) {
    const ImVec2 o(S(1.5f), S(2.0f));
    if (width > 0.0f) {
        TextFit(d, font, ImVec2(pos.x + o.x, pos.y + o.y), IM_COL32(0, 0, 0, static_cast<int>(90 * g_fade)), text, width);
        TextFit(d, font, pos, color, text, width);
    } else {
        Text(d, font, ImVec2(pos.x + o.x, pos.y + o.y), IM_COL32(0, 0, 0, static_cast<int>(90 * g_fade)), text);
        Text(d, font, pos, color, text);
    }
}

// --- Tu skin en 3D --------------------------------------------------------------------
float g_heroYaw = -0.45f;     // giro de la skin del cartel (se cambia arrastrando)
float g_cardYaw = -0.45f;     // y la de la tarjeta
float g_cardZoom = 1.0f;      // 1 = cuerpo entero
float g_cardZoomTarget = 1.0f;
bool g_cardSpin = false;      // girando sola
bool g_skinFullscreen = false;
float g_fullYaw = -0.45f;

// El modelo con la skin actual (o nullptr si todavía no hay skin).
bool SkinModel(const LocalData& data, skin3d::Model& model) {
    if (data.skinPath.empty()) return false;
    const textures::Texture* tex = textures::Get(data.skinPath, data.skinKey);
    if (tex == nullptr) return false;
    model.texture = tex->id;
    model.slim = data.skinSlim;
    model.legacy = data.skinLegacy;
    return true;
}

// Zona donde arrastrar con el mouse gira el personaje.
void DragToRotate(const char* id, ImVec2 min, ImVec2 max, float& yaw) {
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        yaw += ImGui::GetIO().MouseDelta.x * 0.012f;
    }
}

// El menú de la flechita junto a JUGAR (se dibuja al final, encima de todo).
bool g_playMenuOpen = false;
ImVec2 g_playMenuAnchor{0, 0};  // esquina superior izquierda de la flechita

void DrawPlayMenu(ImDrawList* d) {
    if (!g_playMenuOpen) {
        return;
    }
    const Palette& p = P();
    struct Item {
        const char* id;
        const char* key;
        const char* icon;
        int action;
    };
    const Item items[] = {
        {"##pm_folder", "play.menu_folder", kIconFolder, 0},
        {"##pm_logs", "play.menu_logs", kIconNews, 1},
    };
    float width = S(220.0f);
    for (const Item& it : items) {
        width = std::max(width, TextSize(g_fonts.semibold, T(it.key)).x + S(64.0f));
    }
    const float itemH = S(40.0f);
    const float height = itemH * static_cast<float>(std::size(items)) + S(12.0f);
    // Se abre hacia ARRIBA (encima del cartel: ahí no hay otros botones).
    const ImVec2 min(g_playMenuAnchor.x, g_playMenuAnchor.y - height - S(8.0f));
    const ImVec2 max(min.x + width, min.y + height);
    d->AddRectFilled(ImVec2(min.x + S(2), min.y + S(6)), ImVec2(max.x + S(2), max.y + S(6)), Col(ImVec4(0, 0, 0, 1), 0.25f),
                     S(12.0f));
    d->AddRectFilled(min, max, Col(p.card), S(12.0f));
    d->AddRect(min, max, Col(p.border), S(12.0f));
    float y = min.y + S(6.0f);
    bool clickedItem = false;
    for (const Item& it : items) {
        const ImVec2 imin(min.x + S(6.0f), y);
        const ImVec2 imax(max.x - S(6.0f), y + itemH);
        float hover = 0.0f;
        if (Hit(it.id, imin, imax, &hover)) {
            clickedItem = true;
            OpenFolder(it.action == 0 ? DataDir() : DataDir() / L"logs");
        }
        d->AddRectFilled(imin, imax, Col(p.text, 0.07f * hover), S(8.0f));
        icons::Draw(d, it.icon, ImVec2(imin.x + S(18.0f), (imin.y + imax.y) * 0.5f), S(17.0f), Col(p.textDim), S(1.5f));
        Text(d, g_fonts.semibold, ImVec2(imin.x + S(38.0f), (imin.y + imax.y - g_fonts.semibold->FontSize) * 0.5f),
             Col(p.text), T(it.key));
        y += itemH;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool inside = mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y;
    if (clickedItem || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inside && !ImGui::IsAnyItemHovered())) {
        g_playMenuOpen = false;
    }
}

// La versión de Windows ("Windows 11" / "Windows 10"), leída una vez.
const char* WindowsName() {
    static char name[32] = "";
    if (name[0] == 0) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW info{sizeof(info)};
        const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn != nullptr && fn(&info) == 0) {
            std::snprintf(name, sizeof(name), "Windows %d", info.dwMajorVersion >= 10 && info.dwBuildNumber >= 22000 ? 11
                                                                   : static_cast<int>(info.dwMajorVersion));
        } else {
            std::snprintf(name, sizeof(name), "Windows");
        }
    }
    return name;
}

void DrawHero(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data, const std::string& name) {
    const Palette& p = P();
    const float rounding = S(kRound + 4.0f);
    // El cielo pixelado (sigue la hora o el modo de Ajustes).
    sky::Draw(d, min, max, S(5.0f), rounding, Col(p.background), g_skyMode);
    // Sombra suave a la izquierda, detrás del texto (para que se lea aunque
    // pase una nube o sea de día).
    d->AddRectFilledMultiColor(ImVec2(min.x + rounding * 0.5f, min.y + 1), ImVec2(min.x + (max.x - min.x) * 0.6f, max.y - 1),
                               C(ImVec4(0, 0, 0, 1), 0.38f), C(ImVec4(0, 0, 0, 1), 0.0f), C(ImVec4(0, 0, 0, 1), 0.0f),
                               C(ImVec4(0, 0, 0, 1), 0.38f));
    d->AddRect(min, max, C(p.border), rounding, 0, 1.0f);
    const ImU32 white = C(ImVec4(1, 1, 1, 1));
    const ImU32 soft = C(ImVec4(1, 1, 1, 1), 0.82f);
    const float left = min.x + S(36.0f);

    // Tu skin, grande, a la derecha (como en LabyMod), con tu nombre arriba.
    skin3d::Model model;
    if (SkinModel(data, model)) {
        const float heroH = max.y - min.y;
        const float modelH = heroH * 1.02f;  // las piernas quedan un poco cortadas abajo
        const float cx = max.x - std::max(heroH * 0.62f, S(200.0f));
        const ImVec2 mmin(cx - modelH * 0.4f, min.y + heroH * 0.2f);
        const ImVec2 mmax(cx + modelH * 0.4f, mmin.y + modelH);
        DragToRotate("##hero_skin", ImVec2(mmin.x, min.y), ImVec2(mmax.x, max.y), g_heroYaw);
        const bool dragging = ImGui::IsItemActive();
        const float t = static_cast<float>(ImGui::GetTime());
        skin3d::View view;
        view.yaw = g_heroYaw + (dragging ? 0.0f : std::sin(t * 0.35f) * 0.18f);
        view.pitch = 0.06f;
        view.time = t;
        view.alpha = g_fade;
        d->PushClipRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), true);
        const ImVec2 head = skin3d::Draw(d, mmin, mmax, model, view);
        d->PopClipRect();
        // Etiqueta con el nombre (letra pixelada, fondo oscuro como en el juego).
        const ImVec2 ns = TextSize(g_fonts.pixel, name.c_str());
        const ImVec2 tmin(std::round(head.x - ns.x * 0.5f - S(10.0f)), std::round(head.y - ns.y - S(18.0f)));
        const ImVec2 tmax(std::round(head.x + ns.x * 0.5f + S(10.0f)), std::round(head.y - S(6.0f)));
        d->AddRectFilled(tmin, tmax, C(ImVec4(0, 0, 0, 1), 0.42f));
        Text(d, g_fonts.pixel, ImVec2(tmin.x + S(10.0f), tmin.y + (tmax.y - tmin.y - ns.y) * 0.5f), white, name.c_str());
    }

    // Píldora arriba a la izquierda: el estado (Luma cargado / actualizaciones).
    {
        char text[96];
        const ImVec4* dot = &p.success;
        bool spin = false;
        if (data.game.lumaLoaded) {
            std::snprintf(text, sizeof(text), "%s", T("home.luma_loaded"));
        } else if (!g_upd.updatedFrom.empty() && ImGui::GetTime() < 10.0) {
            std::snprintf(text, sizeof(text), T("update.updated_to"), kVersion);
        } else if (g_upd.state == updater::State::Checking) {
            std::snprintf(text, sizeof(text), "%s", T("update.checking"));
            spin = true;
        } else if (g_upd.state == updater::State::Available) {
            std::snprintf(text, sizeof(text), T("update.available"), g_upd.latestVersion.c_str());
            dot = &p.warning;
        } else {
            std::snprintf(text, sizeof(text), "%s", T("home.ready_to_play"));
        }
        const ImVec2 ts = TextSize(g_fonts.tiny, text);
        const ImVec2 pmin(left, min.y + S(22.0f));
        const ImVec2 pmax(pmin.x + ts.x + S(40.0f), pmin.y + S(30.0f));
        d->AddRectFilled(pmin, pmax, C(ImVec4(0, 0, 0, 1), 0.35f), S(15.0f));
        if (spin) {
            Spinner(d, ImVec2(pmin.x + S(16.0f), (pmin.y + pmax.y) * 0.5f), S(5.0f), white);
        } else {
            Dot(d, ImVec2(pmin.x + S(16.0f), (pmin.y + pmax.y) * 0.5f), *dot);
        }
        Text(d, g_fonts.tiny, ImVec2(pmin.x + S(28.0f), (pmin.y + pmax.y - ts.y) * 0.5f), white, text);
    }

    // Bienvenida
    float y = min.y + S(66.0f);
    ShadowText(d, g_fonts.semibold, ImVec2(left, y), soft, T("home.welcome"));
    y += g_fonts.semibold->FontSize + S(2.0f);
    ShadowText(d, g_fonts.hero, ImVec2(left, y), white, name.c_str(), (max.x - min.x) * 0.5f);
    y += g_fonts.hero->FontSize + S(4.0f);
    ShadowText(d, g_fonts.regular, ImVec2(left, y), soft, T("home.subtitle"));

    // Botón JUGAR + flechita del menú
    const PlaySnapshot snap = g_flow.Snapshot();
    const bool busy = g_flow.Busy();
    const float buttonH = S(56.0f);
    const ImVec2 bmin(left, max.y - S(28.0f) - S(22.0f) - buttonH);
    const ImVec2 bmax(bmin.x + S(230.0f), bmin.y + buttonH);
    float hover = 0.0f;
    if (Hit("##play", bmin, bmax, &hover, !busy)) {
        g_flow.Start(g_upd.supportedMinecraft);
    }
    const float press = ImGui::IsItemActive() && !busy ? 1.0f : 0.0f;
    const float lift = S(2.0f) * hover * (1.0f - press);
    const ImVec2 lmin(bmin.x, bmin.y - lift);
    const ImVec2 lmax(bmax.x, bmax.y - lift);
    d->AddRectFilled(ImVec2(lmin.x + S(2), lmin.y + S(6)), ImVec2(lmax.x - S(2), lmax.y + S(6)),
                     C(ImVec4(0, 0, 0, 1), 0.25f + 0.10f * hover), S(12.0f));
    d->AddRectFilled(lmin, lmax, C(busy ? Mix(p.accent, p.card, 0.35f) : p.accent), S(12.0f));
    d->AddRectFilled(lmin, lmax, C(p.onAccent, 0.08f * hover), S(12.0f));
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

    const ImVec2 dmin(bmax.x + S(10.0f), bmin.y);
    const ImVec2 dmax(dmin.x + buttonH, bmax.y);
    float dropHover = 0.0f;
    if (Hit("##play_menu", dmin, dmax, &dropHover)) {
        g_playMenuOpen = !g_playMenuOpen;
        g_playMenuAnchor = dmin;
    }
    d->AddRectFilled(dmin, dmax, C(ImVec4(0, 0, 0, 1), 0.35f + 0.15f * dropHover), S(12.0f));
    d->AddRect(dmin, dmax, C(ImVec4(1, 1, 1, 1), 0.15f), S(12.0f));
    icons::Draw(d, g_playMenuOpen ? icons::kChevronUp : icons::kChevronDown, ImVec2((dmin.x + dmax.x) * 0.5f, (dmin.y + dmax.y) * 0.5f),
                S(20.0f), white, S(2.0f));

    // Debajo: qué juego y versión.
    char info[64];
    if (data.game.gameVersion.empty()) {
        std::snprintf(info, sizeof(info), "Bedrock Edition  ·  %s", T("home.not_installed"));
    } else {
        std::snprintf(info, sizeof(info), "Bedrock Edition  ·  v%s", data.game.gameVersion.c_str());
    }
    ShadowText(d, g_fonts.tiny, ImVec2(left + S(4.0f), bmax.y + S(10.0f)), soft, info);

    // El estado de JUGAR, a la derecha de los botones.
    const float statusX = dmax.x + S(20.0f);
    const float statusW = (max.x - S(30.0f)) - statusX;
    if (snap.state != PlayState::Idle && statusW > S(120.0f)) {
        const ImVec2 smin(statusX - S(12.0f), bmin.y - S(2.0f));
        const ImVec2 smax(std::min(max.x - S(20.0f), statusX + statusW), bmax.y + S(2.0f));
        d->AddRectFilled(smin, smax, C(ImVec4(0, 0, 0, 1), 0.35f), S(12.0f));
        DrawPlayStatus(d, ImVec2(statusX, bmin.y + S(8.0f)), statusW - S(12.0f), snap);
        if (snap.state == PlayState::Unsupported && !busy) {
            if (Button("##play_anyway", ImVec2(statusX, bmin.y + S(26.0f)), T("play.anyway"), false)) {
                g_flow.Start(g_upd.supportedMinecraft, true);
            }
        }
    }
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
        {Page::Settings, kIconSliders, "nav.settings", "home.qa_settings"},
    };
    const float top = min.y + S(54.0f);
    const float bottom = max.y - S(16.0f);
    const float gap = S(10.0f);
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
        const bool roomy = h > S(100.0f);
        const char* title = T(tiles[i].title);
        const ImVec2 ts = TextSize(g_fonts.semibold, title);
        const float blockH = S(28.0f) + S(8.0f) + ts.y + (roomy ? S(4.0f) + g_fonts.tiny->FontSize : 0.0f);
        const float y0 = tmin.y + (h - blockH) * 0.5f;
        icons::Draw(d, tiles[i].icon, ImVec2(cx, y0 + S(14.0f) - S(3.0f) * hover), S(28.0f), C(Mix(p.textDim, p.text, hover)),
                    S(1.8f));
        TextFit(d, g_fonts.semibold, ImVec2(cx - std::min(ts.x, tileW - S(12.0f)) * 0.5f, y0 + S(36.0f)), C(p.text), title,
                tileW - S(12.0f));
        char countText[48];
        const char* sub = T(tiles[i].sub);
        if (tiles[i].page == Page::Skins && !skins::List().empty()) {
            std::snprintf(countText, sizeof(countText), T("skins.count_short"), static_cast<int>(skins::List().size()));
            sub = countText;
        }
        const ImVec2 ss = TextSize(g_fonts.tiny, sub);
        if (roomy && ss.x < tileW - S(12.0f)) {
            Text(d, g_fonts.tiny, ImVec2(cx - ss.x * 0.5f, y0 + S(36.0f) + ts.y + S(4.0f)), C(p.textDim), sub);
        }
    }
}

// "Tu skin": el personaje en 3D, con girar, zoom y pantalla completa.
void DrawSkinCard(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("skin.title"));
    skin3d::Model model;
    if (!SkinModel(data, model)) {
        // Todavía no hay skin: una silueta y cómo conseguirla.
        const float w = max.x - min.x;
        const float h = max.y - min.y - S(56.0f);
        const float u = std::min(w / 14.0f, h / 22.0f);
        const ImVec2 o(min.x + (w - 8.0f * u) * 0.5f, min.y + S(50.0f) + (h - 20.0f * u) * 0.5f);
        const ImU32 c = C(p.textDim, 0.25f);
        auto block = [&](float x0, float y0, float x1, float y1) {
            d->AddRectFilled(ImVec2(o.x + x0 * u, o.y + y0 * u), ImVec2(o.x + x1 * u, o.y + y1 * u), c, u * 0.3f);
        };
        block(2, 0, 6, 4);
        block(2, 4, 6, 10);
        block(0, 4, 2, 10);
        block(6, 4, 8, 10);
        block(2, 10, 4, 16);
        block(4, 10, 6, 16);
        TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(16.0f), o.y + 17.0f * u), C(p.textDim), T("skin.placeholder"), w - S(32.0f));
        return;
    }
    // Botones a la derecha: girar sola, acercar a la cabeza, pantalla completa.
    const float bs = S(34.0f);
    const float bx = max.x - S(14.0f) - bs;
    float by = min.y + S(50.0f);
    struct Btn {
        const char* id;
        const char* icon;
        bool on;
        int action;
    };
    const Btn buttons[] = {
        {"##skin_spin", icons::kRotate, g_cardSpin, 0},
        {"##skin_zoom", icons::kZoomIn, g_cardZoomTarget > 1.0f, 1},
        {"##skin_full", icons::kExpand, false, 2},
    };
    for (const Btn& b : buttons) {
        float hover = 0.0f;
        if (Hit(b.id, ImVec2(bx, by), ImVec2(bx + bs, by + bs), &hover)) {
            if (b.action == 0) g_cardSpin = !g_cardSpin;
            if (b.action == 1) g_cardZoomTarget = g_cardZoomTarget > 1.0f ? 1.0f : 2.4f;
            if (b.action == 2) {
                g_skinFullscreen = true;
                g_fullYaw = g_cardYaw;
            }
        }
        d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bs, by + bs), C(b.on ? p.accent : Mix(p.background, p.cardHover, hover)), S(9.0f));
        d->AddRect(ImVec2(bx, by), ImVec2(bx + bs, by + bs), C(p.border), S(9.0f));
        icons::Draw(d, b.icon, ImVec2(bx + bs * 0.5f, by + bs * 0.5f), S(18.0f), C(b.on ? p.onAccent : p.text), S(1.6f));
        by += bs + S(8.0f);
    }
    // El personaje
    const float dt = Dt();
    g_cardZoom += (g_cardZoomTarget - g_cardZoom) * std::min(1.0f, dt * 8.0f);
    if (g_cardSpin) g_cardYaw += dt * 0.9f;
    const ImVec2 mmin(min.x + S(12.0f), min.y + S(48.0f));
    const ImVec2 mmax(bx - S(8.0f), max.y - S(10.0f));
    DragToRotate("##card_skin", mmin, mmax, g_cardYaw);
    skin3d::View view;
    view.yaw = g_cardYaw;
    view.zoom = g_cardZoom;
    view.time = static_cast<float>(ImGui::GetTime());
    view.alpha = g_fade;
    d->PushClipRect(mmin, mmax, true);
    skin3d::Draw(d, mmin, mmax, model, view);
    d->PopClipRect();
}

// Pantalla completa de la skin (encima de todo; se cierra con la X o Esc).
void DrawSkinFullscreen(ImDrawList* d, ImVec2 display, const LocalData& data) {
    if (!g_skinFullscreen) return;
    skin3d::Model model;
    if (!SkinModel(data, model) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_skinFullscreen = false;
        return;
    }
    const Palette& p = P();
    const ImVec2 min(S(kSidebarWidth) + S(kPad), S(kTitleBarHeight));
    const ImVec2 max(display.x - S(kPad), display.y - S(kPad));
    d->AddRectFilled(ImVec2(0, S(kTitleBarHeight)), display, Col(p.background, 0.75f));
    sky::Draw(d, min, max, S(6.0f), S(kRound + 4.0f), Col(p.background), g_skyMode);
    d->AddRect(min, max, Col(p.border), S(kRound + 4.0f));
    // La X va PRIMERO: en ImGui, si dos botones se superponen, el mouse es del
    // que se registró antes. Antes la zona de girar (toda la pantalla) le
    // "robaba" el clic a la X.
    float hover = 0.0f;
    const ImVec2 xmin(max.x - S(52.0f), min.y + S(16.0f));
    if (Hit("##full_close", xmin, ImVec2(xmin.x + S(36.0f), xmin.y + S(36.0f)), &hover)) {
        g_skinFullscreen = false;
    }
    DragToRotate("##full_skin", min, max, g_fullYaw);
    skin3d::View view;
    view.yaw = g_fullYaw;
    view.time = static_cast<float>(ImGui::GetTime());
    const float h = (max.y - min.y) * 0.86f;
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    skin3d::Draw(d, ImVec2(c.x - h * 0.4f, c.y - h * 0.5f), ImVec2(c.x + h * 0.4f, c.y + h * 0.5f), model, view);
    d->AddRectFilled(xmin, ImVec2(xmin.x + S(36.0f), xmin.y + S(36.0f)), Col(ImVec4(0, 0, 0, 1), 0.35f + 0.2f * hover), S(9.0f));
    icons::Draw(d, icons::kClose, ImVec2(xmin.x + S(18.0f), xmin.y + S(18.0f)), S(16.0f), IM_COL32_WHITE, S(1.8f));
}

void InfoRow(ImDrawList* d, ImVec2 pos, float width, const char* label, const char* value, const ImVec4* dot,
             const char* icon = nullptr) {
    const Palette& p = P();
    float x = pos.x;
    if (icon != nullptr) {
        icons::Draw(d, icon, ImVec2(x + S(9.0f), pos.y + g_fonts.regular->FontSize * 0.5f), S(18.0f), C(p.textDim), S(1.6f));
        x += S(28.0f);
    }
    TextFit(d, g_fonts.regular, ImVec2(x, pos.y), C(p.textDim), label, width * 0.55f - (x - pos.x));
    const ImVec2 vs = TextSize(g_fonts.semibold, value);
    const float vx = pos.x + width - vs.x;
    Text(d, g_fonts.semibold, ImVec2(vx, pos.y), C(p.text), value);
    if (dot != nullptr) {
        Dot(d, ImVec2(vx - S(14.0f), pos.y + vs.y * 0.5f), *dot);
    }
}

void DrawStats(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data) {
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("stats.title"));
    const float x = min.x + S(22.0f);
    const float w = (max.x - min.x) - S(44.0f);
    const int hours = static_cast<int>(data.playSeconds / 3600);
    const int minutes = static_cast<int>((data.playSeconds % 3600) / 60);
    char playtime[48];
    if (hours > 0) {
        std::snprintf(playtime, sizeof(playtime), T("stats.hours"), hours, minutes);
    } else {
        std::snprintf(playtime, sizeof(playtime), T("stats.minutes"), minutes);
    }
    char sessions[24];
    std::snprintf(sessions, sizeof(sessions), "%lld", data.sessions);
    // Última vez que jugaste: hoy / ayer / hace N días.
    char last[48];
    if (data.lastPlayed <= 0) {
        std::snprintf(last, sizeof(last), "%s", T("stats.never"));
    } else {
        const std::time_t now = std::time(nullptr);
        std::tm today{};
        std::tm then{};
        const std::time_t lp = static_cast<std::time_t>(data.lastPlayed);
        localtime_s(&today, &now);
        localtime_s(&then, &lp);
        today.tm_hour = today.tm_min = today.tm_sec = 0;
        then.tm_hour = then.tm_min = then.tm_sec = 0;
        const long long days = static_cast<long long>(std::difftime(std::mktime(&today), std::mktime(&then)) / 86400.0 + 0.5);
        if (days <= 0) std::snprintf(last, sizeof(last), "%s", T("stats.today"));
        else if (days == 1) std::snprintf(last, sizeof(last), "%s", T("stats.yesterday"));
        else std::snprintf(last, sizeof(last), T("stats.days_ago"), static_cast<int>(days));
    }
    const float rowH = std::min(S(38.0f), ((max.y - min.y) - S(62.0f)) / 3.0f);
    float y = min.y + S(56.0f);
    InfoRow(d, ImVec2(x, y), w, T("stats.playtime"), playtime, nullptr, kIconClock);
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("stats.sessions"), sessions, nullptr, kIconSessions);
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("stats.last_played"), last, nullptr, kIconCalendar);
}

// Noticias: las escribimos nosotros (van dentro del launcher). Con el
// actualizador vendrán de GitHub.
struct NewsItem {
    const char* titleKey;
    const char* bodyKey;
    const char* date;
};
constexpr NewsItem kNews[] = {
    {"news.launcher_title", "news.launcher_body", "10.11.2026"},
    {"news.world_title", "news.world_body", "10.11.2026"},
    {"news.hud_title", "news.hud_body", "10.11.2026"},
};

void DrawNews(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("news.title"));
    const float x = min.x + S(22.0f);
    const float w = (max.x - min.x) - S(44.0f);
    const float itemH = S(52.0f);
    float y = min.y + S(54.0f);
    // Si ya bajamos las notas de la última versión (de GitHub), van esas. Cada
    // una puede traer imagen (miniatura a la izquierda) o no (un ícono).
    if (!g_upd.notes.empty()) {
        bool anyImage = false;
        for (const updater::Note& note : g_upd.notes) anyImage |= !note.image.empty();
        const float rowH = anyImage ? S(70.0f) : itemH;
        for (const updater::Note& note : g_upd.notes) {
            if (y + rowH > max.y - S(8.0f)) {
                break;
            }
            float textX = x + S(44.0f);
            const textures::Texture* image = note.image.empty() ? nullptr : textures::Get(note.image);
            if (image != nullptr) {
                const float th = rowH - S(10.0f);
                const float tw = th * 16.0f / 9.0f;
                d->AddImageRounded(image->id, ImVec2(x, y), ImVec2(x + tw, y + th), ImVec2(0, 0), ImVec2(1, 1),
                                   C(ImVec4(1, 1, 1, 1)), S(8.0f));
                textX = x + tw + S(14.0f);
            } else {
                const float cy = y + (anyImage ? (rowH - S(10.0f)) * 0.5f : S(18.0f));
                d->AddCircleFilled(ImVec2(x + S(16.0f), cy), S(16.0f), C(p.background));
                icons::Draw(d, kIconNews, ImVec2(x + S(16.0f), cy), S(17.0f), C(p.textDim), S(1.5f));
            }
            const ImVec2 ds = TextSize(g_fonts.tiny, g_upd.date.c_str());
            Text(d, g_fonts.tiny, ImVec2(x + w - ds.x, y + S(2.0f)), C(p.textDim), g_upd.date.c_str());
            TextFit(d, g_fonts.semibold, ImVec2(textX, y), C(p.text), note.title.c_str(), x + w - textX - ds.x - S(12.0f));
            TextFit(d, g_fonts.tiny, ImVec2(textX, y + g_fonts.semibold->FontSize + S(3.0f)), C(p.textDim),
                    note.body.c_str(), x + w - textX);
            if (&note != &g_upd.notes.back()) {
                d->AddLine(ImVec2(textX, y + rowH - S(5.0f)), ImVec2(x + w, y + rowH - S(5.0f)), C(p.border));
            }
            y += rowH;
        }
        return;
    }
    for (const NewsItem& item : kNews) {
        if (y + itemH > max.y - S(8.0f)) {
            break;  // no entra: la ventana es chica
        }
        d->AddCircleFilled(ImVec2(x + S(16.0f), y + S(18.0f)), S(16.0f), C(p.background));
        icons::Draw(d, kIconNews, ImVec2(x + S(16.0f), y + S(18.0f)), S(17.0f), C(p.textDim), S(1.5f));
        const ImVec2 ds = TextSize(g_fonts.tiny, item.date);
        Text(d, g_fonts.tiny, ImVec2(x + w - ds.x, y + S(2.0f)), C(p.textDim), item.date);
        TextFit(d, g_fonts.semibold, ImVec2(x + S(44.0f), y), C(p.text), T(item.titleKey), w - S(56.0f) - ds.x);
        TextFit(d, g_fonts.tiny, ImVec2(x + S(44.0f), y + g_fonts.semibold->FontSize + S(3.0f)), C(p.textDim),
                T(item.bodyKey), w - S(44.0f));
        y += itemH;
    }
}

void DrawClientInfo(ImDrawList* d, ImVec2 min, ImVec2 max, const GameStatus& status) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("home.client_info"));
    const float x = min.x + S(22.0f);
    const float w = (max.x - min.x) - S(44.0f);
    const float rowH = std::min(S(40.0f), ((max.y - min.y) - S(62.0f)) / 4.0f);
    float y = min.y + S(58.0f);
    // Luma Launcher: versión + estado de las actualizaciones
    {
        char version[32];
        std::snprintf(version, sizeof(version), "v%s", kVersion);
        const char* chip = !updater::OfficialBuild()                       ? T("update.dev_build")
                           : g_upd.state == updater::State::UpToDate       ? T("update.chip_ok")
                           : g_upd.state == updater::State::Available      ? T("update.chip_new")
                                                                           : nullptr;
        const ImVec4* chipColor = g_upd.state == updater::State::Available ? &p.warning
                                  : !updater::OfficialBuild()               ? &p.textDim
                                                                            : &p.success;
        icons::Draw(d, icons::kLumaLogo, ImVec2(x + S(9.0f), y + g_fonts.regular->FontSize * 0.5f), S(18.0f), C(p.textDim));
        Text(d, g_fonts.regular, ImVec2(x + S(28.0f), y), C(p.textDim), "Luma Launcher");
        float vx = x + w;
        if (chip != nullptr) {
            const ImVec2 cs = TextSize(g_fonts.tiny, chip);
            vx -= cs.x;
            Text(d, g_fonts.tiny, ImVec2(vx, y + S(2.0f)), C(*chipColor), chip);
            Dot(d, ImVec2(vx - S(12.0f), y + S(9.0f)), *chipColor);
            vx -= S(28.0f);
        }
        const ImVec2 vs = TextSize(g_fonts.semibold, version);
        Text(d, g_fonts.semibold, ImVec2(vx - vs.x, y), C(p.text), version);
    }
    y += rowH;
    // Minecraft: versión + abierto/cerrado
    {
        icons::Draw(d, kIconAddons, ImVec2(x + S(9.0f), y + g_fonts.regular->FontSize * 0.5f), S(18.0f), C(p.textDim),
                    S(1.6f));
        Text(d, g_fonts.regular, ImVec2(x + S(28.0f), y), C(p.textDim), "Minecraft");
        const char* state = status.running ? T("home.game_running") : T("home.game_closed");
        const ImVec2 ss = TextSize(g_fonts.tiny, state);
        float vx = x + w - ss.x;
        Text(d, g_fonts.tiny, ImVec2(vx, y + S(2.0f)), C(status.running ? p.success : p.textDim), state);
        Dot(d, ImVec2(vx - S(12.0f), y + S(9.0f)), status.running ? p.success : p.textDim);
        vx -= S(28.0f);
        const char* version = status.gameVersion.empty() ? T("home.not_installed") : status.gameVersion.c_str();
        const ImVec2 vs = TextSize(g_fonts.semibold, version);
        Text(d, g_fonts.semibold, ImVec2(vx - vs.x, y), C(p.text), version);
    }
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("home.luma"), status.lumaLoaded ? T("home.luma_loaded") : T("home.luma_unloaded"),
            status.lumaLoaded ? &p.success : &p.textDim, icons::kLumaLogo);
    y += rowH;
    InfoRow(d, ImVec2(x, y), w, T("home.platform"), WindowsName(), nullptr, kIconWindows);
}

// =============================================================================
//  THE MUSIC (Inicio): what's playing, with the SAME code as the game
// =============================================================================
//  client/core/media.cpp reads what Windows says is playing (Spotify, a
//  browser, any app with media controls), its album art and its colors.
//  client/core/audio_viz.cpp listens to how the sound moves (only while the
//  launcher is on screen), and the page breathes with it.
//
//  THE ALBUM'S COLOR: the accent of the whole launcher (the Play button, the
//  active section, the progress) follows the album art, sliding slowly to
//  the new one when the song changes (theme.h SetAccent).
// =============================================================================

// The album art on the GPU. Two of them while a new one fades in over the old.
struct ArtTexture {
    ID3D11ShaderResourceView* view = nullptr;
};
ArtTexture g_art;         // the current one
ArtTexture g_artOld;      // the one fading out
float g_artFade = 1.0f;   // 0..1: how far the new one came in
std::uint32_t g_artVersion = 0;
std::vector<std::uint8_t> g_artPixels;

ImVec4 g_accentNow{0.91f, 0.90f, 0.87f, 1.0f};  // the accent this frame (it slides toward the album's)
bool g_accentReady = false;
std::uint32_t g_shownTrack = 0;
float g_trackIn = 1.0f;   // 0..1: the new song's text coming in
float g_playMorph = 1.0f; // 0 = pause icon, 1 = play icon (animated)
float g_discOut = 0.0f;   // the record sliding out from behind the art while playing
float g_discAngle = 0.0f;
float g_bassSmooth = 0.0f;
float g_levelSmooth = 0.0f;

void ReleaseArt(ArtTexture& t) {
    if (t.view != nullptr) {
        t.view->Release();
        t.view = nullptr;
    }
}

ID3D11ShaderResourceView* MakeArt(const std::vector<std::uint8_t>& rgba) {
    if (g_device == nullptr || rgba.size() < static_cast<std::size_t>(media::kArtSize) * media::kArtSize * 4) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = media::kArtSize;
    desc.Height = media::kArtSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{rgba.data(), media::kArtSize * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture))) {
        g_device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
    }
    return view;
}

// A soft round light (no blur in ImGui: many see-through circles, bigger and
// fainter, add up to a glow).
// The light itself is a picture: white with a perfectly smooth round fade
// (a gaussian), made once, tinted when drawn. Its transparency has a tiny
// bit of noise ("dither"): without it, a dark screen shows the steps of the
// fade as rings.
ID3D11ShaderResourceView* g_lightTex = nullptr;

ID3D11ShaderResourceView* LightTexture() {
    if (g_lightTex != nullptr || g_device == nullptr) return g_lightTex;
    constexpr int N = 256;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(N) * N * 4, 255);
    std::uint32_t seed = 12345u;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / N * 2.0f - 1.0f;
            const float fy = (static_cast<float>(y) + 0.5f) / N * 2.0f - 1.0f;
            const float r2 = fx * fx + fy * fy;
            float a = r2 >= 1.0f ? 0.0f : std::exp(-3.6f * r2) * (1.0f - r2 * r2);
            seed = seed * 1664525u + 1013904223u;
            const float noise = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) / 255.0f;
            a = std::clamp(a + noise * 1.5f, 0.0f, 1.0f);
            rgba[(static_cast<std::size_t>(y) * N + x) * 4 + 3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = N;
    desc.Height = N;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{rgba.data(), N * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture))) {
        g_device->CreateShaderResourceView(texture, nullptr, &g_lightTex);
        texture->Release();
    }
    return g_lightTex;
}

// A soft light: round (or stretched: "rx" x "ry"), its color, its strength.
void SoftLightOval(ImDrawList* d, ImVec2 center, float rx, float ry, const ImVec4& color, float strength) {
    ID3D11ShaderResourceView* tex = LightTexture();
    if (tex == nullptr) return;
    d->AddImage(reinterpret_cast<ImTextureID>(tex), ImVec2(center.x - rx, center.y - ry), ImVec2(center.x + rx, center.y + ry), ImVec2(0, 0),
                ImVec2(1, 1), C(color, std::clamp(strength * 0.55f, 0.0f, 1.0f)));
}
void SoftLight(ImDrawList* d, ImVec2 center, float radius, const ImVec4& color, float strength) {
    SoftLightOval(d, center, radius, radius, color, strength);
}

// THE AMBIENT LIGHT: the album art, very blurred, behind the whole window (like
// the light of the record filling the room), with its colors drifting
// slowly as soft lights that swell with the bass. The blur is cheap: the art
// shrunk to 24x24 pixels (and softened) and stretched over the window: the
// GPU's smooth stretching does the rest.
ArtTexture g_blur;
ArtTexture g_blurOld;
float g_ambient = 0.0f;  // 0..1: the ambient on (there's music)

ID3D11ShaderResourceView* MakeBlur(const std::vector<std::uint8_t>& rgba) {
    constexpr int kSmall = 24;
    const int n = media::kArtSize;
    if (g_device == nullptr || rgba.size() < static_cast<std::size_t>(n) * n * 4) return nullptr;
    std::vector<float> pix(kSmall * kSmall * 3, 0.0f);
    const int block = n / kSmall;
    for (int by = 0; by < kSmall; ++by) {
        for (int bx = 0; bx < kSmall; ++bx) {
            float sum[3] = {0, 0, 0};
            for (int y = by * block; y < (by + 1) * block; y += 2) {
                for (int x = bx * block; x < (bx + 1) * block; x += 2) {
                    const std::uint8_t* px = &rgba[(static_cast<std::size_t>(y) * n + x) * 4];
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                }
            }
            const float count = static_cast<float>((block / 2) * (block / 2));
            for (int c = 0; c < 3; ++c) pix[(by * kSmall + bx) * 3 + c] = sum[c] / count;
        }
    }
    // Softer still: three passes of "each pixel = the average of its neighbors".
    std::vector<float> tmp(pix.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < kSmall; ++y) {
            for (int x = 0; x < kSmall; ++x) {
                for (int c = 0; c < 3; ++c) {
                    float s = 0.0f;
                    int k = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = std::clamp(x + dx, 0, kSmall - 1), yy = std::clamp(y + dy, 0, kSmall - 1);
                            s += pix[(yy * kSmall + xx) * 3 + c];
                            ++k;
                        }
                    }
                    tmp[(y * kSmall + x) * 3 + c] = s / static_cast<float>(k);
                }
            }
        }
        pix.swap(tmp);
    }
    // A little more color (the blur grays it), as a picture for the GPU.
    std::vector<std::uint8_t> out(kSmall * kSmall * 4, 255);
    for (int i = 0; i < kSmall * kSmall; ++i) {
        const float r = pix[i * 3], g = pix[i * 3 + 1], b = pix[i * 3 + 2];
        const float gray = (r + g + b) / 3.0f;
        const float sat = 1.35f;
        out[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(gray + (r - gray) * sat, 0.0f, 255.0f));
        out[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(gray + (g - gray) * sat, 0.0f, 255.0f));
        out[i * 4 + 2] = static_cast<std::uint8_t>(std::clamp(gray + (b - gray) * sat, 0.0f, 255.0f));
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kSmall;
    desc.Height = kSmall;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{out.data(), kSmall * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture))) {
        g_device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
    }
    return view;
}

void DrawAmbient(ImDrawList* d, ImVec2 display, const media::Track& t) {
    const Palette& p = P();
    g_ambient += ((t.present ? 1.0f : 0.0f) - g_ambient) * std::min(1.0f, Dt() * 2.0f);
    g_glass = g_ambient;
    if (g_ambient < 0.01f) return;
    const float now = static_cast<float>(ImGui::GetTime());
    const float big = std::max(display.x, display.y);
    // 1. The blurred art, covering the window, slowly drifting and breathing.
    {
        const float half = big * 0.5f * 1.25f;
        const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
        const ImVec2 a(c.x - half, c.y - half), b(c.x + half, c.y + half);
        auto draw = [&](ID3D11ShaderResourceView* view, float alpha) {
            if (view != nullptr && alpha > 0.0f) {
                d->AddImage(reinterpret_cast<ImTextureID>(view), a, b, ImVec2(0, 0), ImVec2(1, 1), Col(ImVec4(1, 1, 1, 1), alpha));
            }
        };
        if (g_blur.view == nullptr) {
            d->AddRectFilled(ImVec2(0, 0), display, Col(p.accent, 0.35f * g_ambient));
        }
        draw(g_blurOld.view, g_ambient * (1.0f - g_artFade));
        draw(g_blur.view, g_ambient * (g_blurOld.view != nullptr ? g_artFade : 1.0f));
    }
    // 2. Its colors, as big soft lights resting in the corners (they glow a
    // little brighter with the bass; they don't move).
    static const float kSpots[5][2] = {{0.12f, 0.18f}, {0.88f, 0.22f}, {0.20f, 0.88f}, {0.82f, 0.85f}, {0.50f, 0.50f}};
    for (int i = 0; i < std::clamp(t.paletteCount, 0, 5); ++i) {
        const ImVec2 c(display.x * kSpots[i][0], display.y * kSpots[i][1]);
        const ImVec4 col(t.palette[i][0], t.palette[i][1], t.palette[i][2], 1.0f);
        SoftLight(d, c, big * 0.55f, col, (0.6f + 0.25f * g_bassSmooth) * g_ambient);
    }
    (void)now;
    // 3. Shade, so the words read: darker at the bottom and the edges.
    const bool dark = CurrentTheme() == ThemeKind::Dark;
    const ImVec4 shade = dark ? ImVec4(0.05f, 0.05f, 0.06f, 1.0f) : ImVec4(0.97f, 0.96f, 0.94f, 1.0f);
    const float top = (dark ? 0.42f : 0.50f) * g_ambient, bottom = (dark ? 0.72f : 0.70f) * g_ambient;
    d->AddRectFilledMultiColor(ImVec2(0, 0), display, Col(shade, top), Col(shade, top), Col(shade, bottom), Col(shade, bottom));
    const float edge = big * 0.18f;
    d->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(edge, display.y), Col(shade, 0.35f * g_ambient), Col(shade, 0.0f),
                               Col(shade, 0.0f), Col(shade, 0.35f * g_ambient));
    d->AddRectFilledMultiColor(ImVec2(display.x - edge, 0), display, Col(shade, 0.0f), Col(shade, 0.35f * g_ambient),
                               Col(shade, 0.35f * g_ambient), Col(shade, 0.0f));
}

// Once per frame: the music, its art and the accent.
void UpdateMusic(const media::Track& t, bool visible) {
    media::Want(media::kMenu, visible);
    audioviz::Want(visible && g_page == Page::Home && t.present && t.playing);
    // A new album art: it fades in over the old one.
    std::uint32_t version = g_artVersion;
    if (media::TakeArt(g_artVersion, g_artPixels, version)) {
        g_artVersion = version;
        ID3D11ShaderResourceView* view = MakeArt(g_artPixels);
        if (view != nullptr) {
            ReleaseArt(g_artOld);
            g_artOld = g_art;
            g_art.view = view;
            ReleaseArt(g_blurOld);
            g_blurOld = g_blur;
            g_blur.view = MakeBlur(g_artPixels);
            g_artFade = 0.0f;
        }
    }
    if (!t.present || t.artVersion == 0) {
        if (g_art.view != nullptr && g_artFade >= 1.0f) {
            ReleaseArt(g_artOld);
            g_artOld = g_art;
            g_art = ArtTexture{};
            ReleaseArt(g_blurOld);
            g_blurOld = g_blur;
            g_blur = ArtTexture{};
            g_artFade = 0.0f;
            g_artVersion = 0;
        }
    }
    g_artFade = std::min(1.0f, g_artFade + Dt() * 2.2f);
    if (g_artFade >= 1.0f) {
        ReleaseArt(g_artOld);
        ReleaseArt(g_blurOld);
    }
    // A new song: its name comes in.
    if (t.trackId != g_shownTrack) {
        g_shownTrack = t.trackId;
        g_trackIn = 0.0f;
    }
    g_trackIn = std::min(1.0f, g_trackIn + Dt() * 2.5f);
    // The accent: toward the album's color (slowly), or the theme's own.
    const ImVec4 target = t.present ? ImVec4(t.accent[0], t.accent[1], t.accent[2], 1.0f) : ImVec4(0, 0, 0, 0);
    if (t.present) {
        if (!g_accentReady) {
            g_accentNow = target;
            g_accentReady = true;
        }
        const float k = 1.0f - std::exp(-Dt() * 2.5f);
        g_accentNow = Mix(g_accentNow, target, k);
        SetAccent(&g_accentNow);
    } else {
        g_accentReady = false;
        SetAccent(nullptr);
    }
    // The play / pause icon and the record.
    const float dt = Dt();
    g_playMorph += ((t.playing ? 0.0f : 1.0f) - g_playMorph) * std::min(1.0f, dt * 14.0f);
    g_discOut += ((t.present && t.playing ? 1.0f : 0.0f) - g_discOut) * std::min(1.0f, dt * 4.0f);
    if (t.playing) g_discAngle += dt * 1.6f;
    // The sound (smoothed: the glow breathes, it doesn't flicker).
    const audioviz::Levels lv = audioviz::Get();
    const float bass = lv.live && t.playing ? std::max(lv.bass, lv.kick * 0.8f) : 0.0f;
    const float level = lv.live && t.playing ? lv.level : 0.0f;
    g_bassSmooth += (bass - g_bassSmooth) * std::min(1.0f, dt * (bass > g_bassSmooth ? 18.0f : 5.0f));
    g_levelSmooth += (level - g_levelSmooth) * std::min(1.0f, dt * 6.0f);
}

void FormatTime(double seconds, char* out, std::size_t size) {
    const int s = std::max(0, static_cast<int>(seconds));
    std::snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

// A round button with an icon; pressing it sinks it a little.
bool RoundButton(ImDrawList* d, const char* id, ImVec2 center, float radius, const char* icon, float iconSize, bool primary,
                 bool enabled = true) {
    const Palette& p = P();
    float hover = 0.0f;
    const bool clicked = Hit(id, ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius), &hover,
                             enabled);
    const float press = Anim(ImGui::GetID(id) + 11, ImGui::IsItemActive() && enabled, 22.0f);
    const float r = radius * (1.0f - 0.07f * press) * (1.0f + 0.04f * hover);
    const float a = enabled ? 1.0f : 0.35f;
    if (primary) {
        d->AddCircleFilled(center, r + S(6.0f) * hover, C(p.text, 0.08f * hover), 48);
        d->AddCircleFilled(center, r, C(p.text, a), 48);
        icons::Draw(d, icon, center, iconSize, C(p.background, a));
    } else {
        d->AddCircleFilled(center, r, C(Mix(p.card, p.cardHover, hover * 1.5f), a), 48);
        icons::Draw(d, icon, center, iconSize, C(Mix(p.textDim, p.text, hover), a));
    }
    return clicked;
}

// "Good morning/afternoon/evening", by the clock.
const char* GreetingKey() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    if (local.tm_hour < 5) return "home.good_night";
    if (local.tm_hour < 12) return "home.good_morning";
    if (local.tm_hour < 20) return "home.good_afternoon";
    return "home.good_evening";
}

// How far a block of the page came in (0..1): they come in one after another.
float Stagger(int index) {
    const float t = std::clamp(g_pageFade * 1.6f - static_cast<float>(index) * 0.18f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void DrawNowPlaying(ImDrawList* d, ImVec2 min, ImVec2 max, const media::Track& t) {
    const Palette& p = P();
    const float rounding = S(kRound + 6.0f);
    Card(d, min, max);
    d->PushClipRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), true);
    const float h = max.y - min.y;
    const float artSize = std::min(h - S(56.0f), S(300.0f));
    const ImVec2 artMin(min.x + S(28.0f), min.y + (h - artSize) * 0.5f);
    const ImVec2 artMax(artMin.x + artSize, artMin.y + artSize);
    const ImVec2 artCenter((artMin.x + artMax.x) * 0.5f, (artMin.y + artMax.y) * 0.5f);

    if (!t.present) {
        // Nothing playing: a quiet invitation, breathing.
        const float breathe = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.4f);
        SoftLight(d, artCenter, artSize * 0.9f, p.text, 0.25f + 0.15f * breathe);
        d->AddRectFilled(artMin, artMax, C(p.background), S(18.0f));
        d->AddRect(artMin, artMax, C(p.border), S(18.0f));
        icons::Draw(d, icons::kMusic, artCenter, artSize * 0.28f, C(p.textDim, 0.6f + 0.3f * breathe), S(1.6f));
        const float x = artMax.x + S(36.0f);
        const float w = max.x - x - S(28.0f);
        float y = artCenter.y - S(40.0f);
        TextFit(d, g_fonts.hero, ImVec2(x, y), C(p.text), T("music.nothing_title"), w);
        y += g_fonts.hero->FontSize + S(8.0f);
        TextWrap(d, g_fonts.regular, ImVec2(x, y), C(p.textDim), T("music.nothing_text"), w);
        d->PopClipRect();
        return;
    }

    // The light behind the art: the album's color, breathing with the bass.
    SoftLight(d, artCenter, artSize * (0.95f + 0.22f * g_bassSmooth), p.accent, 0.55f + 0.9f * g_bassSmooth);

    // The record: it slides out from behind the art while playing, and turns.
    if (g_discOut > 0.01f) {
        const float r = artSize * 0.46f;
        const ImVec2 c(artCenter.x + artSize * 0.30f * g_discOut, artCenter.y);
        d->AddCircleFilled(c, r, C(ImVec4(0.06f, 0.06f, 0.07f, 1.0f), g_discOut), 64);
        for (int g = 1; g <= 5; ++g) {
            d->AddCircle(c, r * (0.48f + 0.09f * static_cast<float>(g)), C(ImVec4(1, 1, 1, 1), 0.05f * g_discOut), 64, 1.0f);
        }
        // a light reflection that turns with it
        d->PathArcTo(c, r * 0.82f, g_discAngle, g_discAngle + 0.9f, 20);
        d->PathStroke(C(ImVec4(1, 1, 1, 1), 0.12f * g_discOut), ImDrawFlags_None, S(3.0f));
        d->PathArcTo(c, r * 0.82f, g_discAngle + 3.14159f, g_discAngle + 4.04f, 20);
        d->PathStroke(C(ImVec4(1, 1, 1, 1), 0.08f * g_discOut), ImDrawFlags_None, S(3.0f));
        d->AddCircleFilled(c, r * 0.30f, C(p.accent, g_discOut), 48);
        d->AddCircleFilled(c, r * 0.05f, C(ImVec4(0.06f, 0.06f, 0.07f, 1.0f), g_discOut), 16);
    }

    // The art (the new one fades in over the old, growing a little).
    const float pop = 0.94f + 0.06f * (1.0f - (1.0f - g_artFade) * (1.0f - g_artFade));
    auto drawArt = [&](ID3D11ShaderResourceView* view, float alpha, float scale) {
        const float half = artSize * 0.5f * scale;
        const ImVec2 a(artCenter.x - half, artCenter.y - half), b(artCenter.x + half, artCenter.y + half);
        if (view == nullptr) {
            d->AddRectFilled(a, b, C(p.accent, alpha), S(18.0f));
            icons::Draw(d, icons::kMusic, artCenter, artSize * 0.26f, C(p.onAccent, alpha * 0.7f), S(1.6f));
            return;
        }
        d->AddImageRounded(reinterpret_cast<ImTextureID>(view), a, b, ImVec2(0, 0), ImVec2(1, 1), C(ImVec4(1, 1, 1, 1), alpha),
                           S(18.0f));
    };
    // (a soft shadow under it)
    d->AddRectFilled(ImVec2(artMin.x + S(4.0f), artMin.y + S(10.0f)), ImVec2(artMax.x - S(4.0f), artMax.y + S(10.0f)),
                     C(ImVec4(0, 0, 0, 1), 0.28f), S(20.0f));
    if (g_artOld.view != nullptr && g_artFade < 1.0f) drawArt(g_artOld.view, 1.0f, 1.0f);
    drawArt(g_art.view, g_artFade < 1.0f ? g_artFade : 1.0f, pop);
    d->AddRect(artMin, artMax, C(ImVec4(1, 1, 1, 1), 0.06f), S(18.0f));

    // --- The words -----------------------------------------------------------------------
    const float x = artMax.x + S(44.0f) + artSize * 0.24f * g_discOut;
    const float w = max.x - x - S(30.0f);
    const float slide = (1.0f - (1.0f - (1.0f - g_trackIn) * (1.0f - g_trackIn))) * S(14.0f);
    const float textA = g_trackIn;
    float y = artMin.y + S(6.0f);
    // "Now playing" + a little equalizer that dances with the sound.
    {
        const char* label = t.playing ? T("music.now_playing") : T("music.paused");
        Text(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), label);
        const float lx = x + TextSize(g_fonts.tiny, label).x + S(12.0f);
        const audioviz::Levels lv = audioviz::Get();
        const float now = static_cast<float>(ImGui::GetTime());
        for (int b = 0; b < 4; ++b) {
            float v = 0.15f;
            if (t.playing) {
                v = lv.live ? lv.fast[std::min(audioviz::kBands - 1, 2 + b * 9)]
                            : 0.35f + 0.3f * std::sin(now * (5.0f + static_cast<float>(b)) + static_cast<float>(b));
            }
            const float bh = S(3.0f) + S(11.0f) * std::clamp(v, 0.0f, 1.0f);
            const float bx = lx + static_cast<float>(b) * S(5.0f);
            const float by = y + g_fonts.tiny->FontSize * 0.5f + S(7.0f);
            d->AddRectFilled(ImVec2(bx, by - bh), ImVec2(bx + S(3.0f), by), C(p.accent), S(1.5f));
        }
        if (t.app[0] != '\0') {
            const ImVec2 as = TextSize(g_fonts.tiny, t.app);
            const ImVec2 cmin(max.x - S(30.0f) - as.x - S(20.0f), y - S(4.0f));
            const ImVec2 cmax(max.x - S(30.0f), y + as.y + S(4.0f));
            d->AddRectFilled(cmin, cmax, C(p.background, 0.7f), S(12.0f));
            Text(d, g_fonts.tiny, ImVec2(cmin.x + S(10.0f), y), C(p.textDim), t.app);
        }
    }
    y += g_fonts.tiny->FontSize + S(14.0f);
    TextFit(d, g_fonts.hero, ImVec2(x + slide, y), C(p.text, textA), t.title, w);
    y += g_fonts.hero->FontSize + S(6.0f);
    {
        char line[340];
        if (t.album[0] != '\0' && std::strcmp(t.album, t.title) != 0) {
            std::snprintf(line, sizeof(line), "%s  ·  %s", t.artist, t.album);
        } else {
            std::snprintf(line, sizeof(line), "%s", t.artist);
        }
        TextFit(d, g_fonts.semibold, ImVec2(x + slide * 1.6f, y), C(p.textDim, textA), line, w);
    }

    // --- The progress (click to jump) ------------------------------------------------------
    const float barY = artMax.y - S(88.0f);
    if (t.duration > 0.0) {
        const float frac = static_cast<float>(std::clamp(t.position / t.duration, 0.0, 1.0));
        const ImVec2 bmin(x, barY - S(10.0f));
        const ImVec2 bmax(x + w, barY + S(10.0f));
        float hover = 0.0f;
        const bool clicked = Hit("##seek", bmin, bmax, &hover, t.canSeek);
        if (clicked) {
            const float to = std::clamp((ImGui::GetIO().MousePos.x - x) / std::max(1.0f, w), 0.0f, 1.0f);
            media::Seek(t.duration * to);
        }
        const float th = S(5.0f) + S(3.0f) * hover;
        d->AddRectFilled(ImVec2(x, barY - th * 0.5f), ImVec2(x + w, barY + th * 0.5f), C(p.text, 0.12f), th * 0.5f);
        d->AddRectFilled(ImVec2(x, barY - th * 0.5f), ImVec2(x + w * frac, barY + th * 0.5f), C(p.accent), th * 0.5f);
        if (hover > 0.01f) d->AddCircleFilled(ImVec2(x + w * frac, barY), S(7.0f) * hover, C(p.text), 20);
        char a[16], b[16];
        FormatTime(t.position, a, sizeof(a));
        FormatTime(t.duration, b, sizeof(b));
        Text(d, g_fonts.tiny, ImVec2(x, barY + S(10.0f)), C(p.textDim), a);
        Text(d, g_fonts.tiny, ImVec2(x + w - TextSize(g_fonts.tiny, b).x, barY + S(10.0f)), C(p.textDim), b);
    }

    // --- The controls ---------------------------------------------------------------------
    const float cy = artMax.y - S(30.0f);
    if (RoundButton(d, "##prev", ImVec2(x + S(24.0f), cy), S(24.0f), icons::kSkipBackFilled, S(18.0f), false, t.canPrevious)) {
        media::Previous();
    }
    {
        const ImVec2 c(x + S(24.0f) + S(70.0f), cy);
        // The play button's halo breathes with the music.
        if (t.playing) d->AddCircleFilled(c, S(30.0f) + S(10.0f) * g_bassSmooth, C(p.accent, 0.18f + 0.25f * g_bassSmooth), 48);
        if (RoundButton(d, "##playpause", c, S(30.0f), g_playMorph > 0.5f ? icons::kPlayFilled : icons::kPauseFilled, S(22.0f), true,
                        t.canPlayPause)) {
            media::PlayPause();
        }
    }
    if (RoundButton(d, "##next", ImVec2(x + S(24.0f) + S(140.0f), cy), S(24.0f), icons::kSkipForwardFilled, S(18.0f), false, t.canNext)) {
        media::Next();
    }
    d->PopClipRect();
}

// The bar at the bottom: PLAY, and how the game and Luma are.
void DrawPlayBar(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data) {
    const Palette& p = P();
    Card(d, min, max);
    const PlaySnapshot snap = g_flow.Snapshot();
    const bool busy = g_flow.Busy();
    const float h = max.y - min.y;
    const float buttonH = std::min(S(56.0f), h - S(24.0f));
    const ImVec2 bmin(min.x + S(18.0f), min.y + (h - buttonH) * 0.5f);
    const ImVec2 bmax(bmin.x + S(220.0f), bmin.y + buttonH);
    float hover = 0.0f;
    if (Hit("##play", bmin, bmax, &hover, !busy)) {
        g_flow.Start(g_upd.supportedMinecraft);
    }
    const float press = Anim(ImGui::GetID("##play") + 5, ImGui::IsItemActive() && !busy, 22.0f);
    // Waiting for you: a slow breathing halo around it.
    const float now = static_cast<float>(ImGui::GetTime());
    if (!busy && !data.game.lumaLoaded) {
        const float breathe = 0.5f + 0.5f * std::sin(now * 2.2f);
        const float g = S(3.0f) + S(5.0f) * breathe;
        d->AddRectFilled(ImVec2(bmin.x - g, bmin.y - g), ImVec2(bmax.x + g, bmax.y + g), C(p.accent, 0.10f + 0.12f * breathe),
                         S(14.0f) + g);
    }
    const float sink = S(2.0f) * press;
    const ImVec2 lmin(bmin.x + sink, bmin.y + sink), lmax(bmax.x - sink, bmax.y - sink);
    d->AddRectFilled(lmin, lmax, C(busy ? Mix(p.accent, p.card, 0.35f) : p.accent), S(14.0f));
    d->AddRectFilled(lmin, lmax, C(p.onAccent, 0.07f * hover), S(14.0f));
    const char* label = T("home.play");
    const ImVec2 ls = TextSize(g_fonts.title, label);
    const float contentW = S(22.0f) + S(14.0f) + ls.x;
    const float cx = (lmin.x + lmax.x) * 0.5f - contentW * 0.5f;
    const float cy = (lmin.y + lmax.y) * 0.5f;
    if (busy) {
        Spinner(d, ImVec2(cx + S(11.0f), cy), S(9.0f), C(p.onAccent));
    } else {
        icons::Draw(d, kIconPlay, ImVec2(cx + S(11.0f), cy), S(22.0f), C(p.onAccent));
    }
    Text(d, g_fonts.title, ImVec2(cx + S(36.0f), cy - ls.y * 0.5f), C(p.onAccent), label);

    // The little menu (open the Luma folder, the logs).
    const ImVec2 dmin(bmax.x + S(8.0f), bmin.y);
    const ImVec2 dmax(dmin.x + buttonH, bmax.y);
    float dropHover = 0.0f;
    if (Hit("##play_menu", dmin, dmax, &dropHover)) {
        g_playMenuOpen = !g_playMenuOpen;
        g_playMenuAnchor = dmin;
    }
    d->AddRectFilled(dmin, dmax, C(Mix(p.background, p.cardHover, dropHover)), S(14.0f));
    d->AddRect(dmin, dmax, C(p.border), S(14.0f));
    icons::Draw(d, g_playMenuOpen ? icons::kChevronUp : icons::kChevronDown, ImVec2((dmin.x + dmax.x) * 0.5f, (dmin.y + dmax.y) * 0.5f),
                S(18.0f), C(p.text), S(2.0f));

    // How it is: the play steps while they run, else the game and Luma.
    const float sx = dmax.x + S(24.0f);
    const float sw = max.x - sx - S(18.0f);
    if (snap.state != PlayState::Idle) {
        DrawPlayStatus(d, ImVec2(sx, min.y + h * 0.5f - S(18.0f)), sw, snap);
        if (snap.state == PlayState::Unsupported && !busy && sw > S(200.0f)) {
            if (Button("##play_anyway", ImVec2(max.x - S(18.0f) - S(160.0f), min.y + (h - S(38.0f)) * 0.5f), T("play.anyway"), false)) {
                g_flow.Start(g_upd.supportedMinecraft, true);
            }
        }
        return;
    }
    char game[96];
    if (data.game.gameVersion.empty()) {
        std::snprintf(game, sizeof(game), "Minecraft  ·  %s", T("home.not_installed"));
    } else {
        std::snprintf(game, sizeof(game), "Minecraft  ·  %s", data.game.gameVersion.c_str());
    }
    const float ty = min.y + h * 0.5f - g_fonts.semibold->FontSize;
    TextFit(d, g_fonts.semibold, ImVec2(sx, ty), C(p.text), game, sw);
    const bool loaded = data.game.lumaLoaded;
    const char* state = loaded ? T("home.luma_loaded") : T("home.ready_to_play");
    Dot(d, ImVec2(sx + S(6.0f), ty + g_fonts.semibold->FontSize + S(12.0f)), loaded ? p.success : p.accent);
    TextFit(d, g_fonts.tiny, ImVec2(sx + S(18.0f), ty + g_fonts.semibold->FontSize + S(4.0f)), C(p.textDim), state, sw - S(18.0f));
}

// =============================================================================
//  THE STAGE ("Escenario"): the launcher IS the song
// =============================================================================
//  No sidebar, no cards. The whole window is the music (DrawAmbient: the
//  album art blurred, its colors drifting); on it, floating:
//    - the album art and the song, in the middle;
//    - the dock: a glass pill at the bottom with the sections (the lit one
//      slides to the one you pick);
//    - the PLAY orb, bottom right: a glowing ball that breathes and beats
//      with the music;
//    - the game's state, a small pill at the top.
//  The other sections rise as a glass sheet over the stage (the music keeps
//  playing behind it, a little darker).
// =============================================================================

constexpr float kDockH = 54.0f;
constexpr float kDockBottom = 26.0f;
float g_sheet = 0.0f;           // 0..1: the section sheet up
float g_dockLit = 0.0f;         // where the lit pill of the dock is (index, animated)
Page g_sheetPage = Page::Settings;  // the page the sheet shows (kept while it goes down)

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// The album art (or a placeholder) at "center", "size" wide, with its old
// one fading out.
void DrawArtAt(ImDrawList* d, ImVec2 center, float size, float alpha) {
    const Palette& p = P();
    const float pop = 0.94f + 0.06f * EaseOutCubic(g_artFade);
    auto one = [&](ID3D11ShaderResourceView* view, float a, float scale) {
        const float half = size * 0.5f * scale;
        const ImVec2 mn(center.x - half, center.y - half), mx(center.x + half, center.y + half);
        if (view == nullptr) {
            d->AddRectFilled(mn, mx, C(p.accent, a * alpha), S(22.0f));
            icons::Draw(d, icons::kMusic, center, size * 0.26f, C(p.onAccent, a * alpha * 0.7f), S(1.6f));
            return;
        }
        d->AddImageRounded(reinterpret_cast<ImTextureID>(view), mn, mx, ImVec2(0, 0), ImVec2(1, 1), C(ImVec4(1, 1, 1, 1), a * alpha),
                           S(22.0f));
    };
    if (g_artOld.view != nullptr && g_artFade < 1.0f) one(g_artOld.view, 1.0f, 1.0f);
    one(g_art.view, g_artOld.view != nullptr ? g_artFade : 1.0f, pop);
    const float half = size * 0.5f;
    d->AddRect(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), C(ImVec4(1, 1, 1, 1), 0.08f * alpha),
               S(22.0f));
}

// The song on the stage: art + record + words + progress + controls.
void DrawStageSong(ImDrawList* d, ImVec2 min, ImVec2 max, const media::Track& t, float alpha) {
    const Palette& p = P();
    const float W = max.x - min.x, H = max.y - min.y;
    const float now = static_cast<float>(ImGui::GetTime());
    const float artSize = std::clamp(std::min(H * 0.86f, W * 0.30f), S(170.0f), S(330.0f));
    const float infoW = std::clamp(W * 0.42f, S(300.0f), S(520.0f));
    const float discReach = artSize * 0.26f;
    const float groupW = artSize + discReach + S(54.0f) + infoW;
    const float x0 = min.x + (W - groupW) * 0.5f;
    const float floatY = 0.0f;  // (still: nothing sways)
    const ImVec2 artC(x0 + artSize * 0.5f, min.y + H * 0.5f + floatY);

    if (!t.present) {
        // Nothing playing: the stage waits, breathing.
        const float breathe = 0.5f + 0.5f * std::sin(now * 1.3f);
        const ImVec2 c(min.x + W * 0.5f, min.y + H * 0.42f);
        SoftLight(d, c, S(160.0f) + S(30.0f) * breathe, p.text, (0.25f + 0.15f * breathe) * alpha);
        icons::Draw(d, icons::kMusic, c, S(54.0f), C(p.text, (0.55f + 0.35f * breathe) * alpha), S(1.4f));
        const char* title = T("music.nothing_title");
        const ImVec2 ts = TextSize(g_fonts.hero, title);
        Text(d, g_fonts.hero, ImVec2(c.x - ts.x * 0.5f, c.y + S(56.0f)), C(p.text, alpha), title);
        const float tw = std::min(W * 0.6f, S(560.0f));
        const char* text = T("music.nothing_text");
        const ImVec2 xs = g_fonts.regular->CalcTextSizeA(g_fonts.regular->FontSize, FLT_MAX, tw, text);
        TextWrap(d, g_fonts.regular, ImVec2(c.x - std::min(xs.x, tw) * 0.5f, c.y + S(56.0f) + ts.y + S(10.0f)), C(p.textDim, alpha), text, tw);
        return;
    }

    // The art's own light, breathing with the bass.
    SoftLight(d, artC, artSize * 1.15f, p.accent, (0.75f + 0.45f * g_bassSmooth) * alpha);
    // The record behind it.
    if (g_discOut > 0.01f) {
        const float r = artSize * 0.47f;
        const ImVec2 c(artC.x + discReach * g_discOut, artC.y);
        const float a = g_discOut * alpha;
        d->AddCircleFilled(c, r, C(ImVec4(0.05f, 0.05f, 0.06f, 1.0f), a), 72);
        for (int g = 1; g <= 6; ++g) d->AddCircle(c, r * (0.42f + 0.085f * static_cast<float>(g)), C(ImVec4(1, 1, 1, 1), 0.045f * a), 72, 1.0f);
        d->PathArcTo(c, r * 0.80f, g_discAngle, g_discAngle + 0.9f, 20);
        d->PathStroke(C(ImVec4(1, 1, 1, 1), 0.13f * a), ImDrawFlags_None, S(3.0f));
        d->PathArcTo(c, r * 0.80f, g_discAngle + 3.14159f, g_discAngle + 4.04f, 20);
        d->PathStroke(C(ImVec4(1, 1, 1, 1), 0.09f * a), ImDrawFlags_None, S(3.0f));
        d->AddCircleFilled(c, r * 0.30f, C(p.accent, a), 48);
        d->AddCircleFilled(c, r * 0.05f, C(ImVec4(0.05f, 0.05f, 0.06f, 1.0f), a), 16);
    }
    // (its shadow, then the art)
    SoftLightOval(d, ImVec2(artC.x, artC.y + artSize * 0.10f), artSize * 0.78f, artSize * 0.70f, ImVec4(0, 0, 0, 1), 1.1f * alpha);
    DrawArtAt(d, artC, artSize, alpha);

    // --- The words, the progress, the controls -------------------------------------------
    const float x = x0 + artSize + discReach * g_discOut + S(54.0f);
    const float w = infoW;
    const float slide = (1.0f - EaseOutCubic(g_trackIn)) * S(18.0f);
    const float ta = g_trackIn * alpha;
    float y = artC.y - S(112.0f);
    {
        const char* label = t.playing ? T("music.now_playing") : T("music.paused");
        Text(d, g_fonts.tiny, ImVec2(x, y), C(p.text, 0.7f * alpha), label);
        // a little equalizer dancing with the sound
        const float lx = x + TextSize(g_fonts.tiny, label).x + S(12.0f);
        const audioviz::Levels lv = audioviz::Get();
        for (int b = 0; b < 4; ++b) {
            float v = 0.15f;
            if (t.playing) {
                v = lv.live ? lv.fast[std::min(audioviz::kBands - 1, 2 + b * 9)]
                            : 0.35f + 0.3f * std::sin(now * (5.0f + static_cast<float>(b)) + static_cast<float>(b));
            }
            const float bh = S(3.0f) + S(11.0f) * std::clamp(v, 0.0f, 1.0f);
            const float bx = lx + static_cast<float>(b) * S(5.0f);
            const float by = y + g_fonts.tiny->FontSize * 0.5f + S(7.0f);
            d->AddRectFilled(ImVec2(bx, by - bh), ImVec2(bx + S(3.0f), by), C(p.accent, alpha), S(1.5f));
        }
        if (t.app[0] != '\0') {
            const float ax = lx + S(32.0f);
            const ImVec2 as = TextSize(g_fonts.tiny, t.app);
            d->AddRectFilled(ImVec2(ax, y - S(3.0f)), ImVec2(ax + as.x + S(18.0f), y + as.y + S(3.0f)), C(ImVec4(1, 1, 1, 1), 0.10f * alpha),
                             S(12.0f));
            Text(d, g_fonts.tiny, ImVec2(ax + S(9.0f), y), C(p.text, 0.8f * alpha), t.app);
        }
    }
    y += g_fonts.tiny->FontSize + S(12.0f);
    TextFit(d, g_fonts.hero, ImVec2(x + slide, y), C(p.text, ta), t.title, w);
    y += g_fonts.hero->FontSize + S(4.0f);
    {
        char line[340];
        if (t.album[0] != '\0' && std::strcmp(t.album, t.title) != 0) {
            std::snprintf(line, sizeof(line), "%s  ·  %s", t.artist, t.album);
        } else {
            std::snprintf(line, sizeof(line), "%s", t.artist);
        }
        TextFit(d, g_fonts.semibold, ImVec2(x + slide * 1.6f, y), C(p.text, 0.72f * ta), line, w);
    }
    y += g_fonts.semibold->FontSize + S(26.0f);
    if (t.duration > 0.0) {
        const float frac = static_cast<float>(std::clamp(t.position / t.duration, 0.0, 1.0));
        float hover = 0.0f;
        if (Hit("##seek", ImVec2(x, y - S(10.0f)), ImVec2(x + w, y + S(10.0f)), &hover, t.canSeek)) {
            media::Seek(t.duration * std::clamp((ImGui::GetIO().MousePos.x - x) / std::max(1.0f, w), 0.0f, 1.0f));
        }
        const float th = S(5.0f) + S(3.0f) * hover;
        d->AddRectFilled(ImVec2(x, y - th * 0.5f), ImVec2(x + w, y + th * 0.5f), C(ImVec4(1, 1, 1, 1), 0.16f * alpha), th * 0.5f);
        // the filled part glows a little
        d->AddRectFilled(ImVec2(x, y - th * 1.4f), ImVec2(x + w * frac, y + th * 1.4f), C(p.accent, 0.12f * alpha), th);
        d->AddRectFilled(ImVec2(x, y - th * 0.5f), ImVec2(x + w * frac, y + th * 0.5f), C(p.accent, alpha), th * 0.5f);
        d->AddCircleFilled(ImVec2(x + w * frac, y), S(4.0f) + S(4.0f) * hover, C(p.text, alpha), 20);
        char a[16], b[16];
        FormatTime(t.position, a, sizeof(a));
        FormatTime(t.duration, b, sizeof(b));
        Text(d, g_fonts.tiny, ImVec2(x, y + S(10.0f)), C(p.text, 0.6f * alpha), a);
        Text(d, g_fonts.tiny, ImVec2(x + w - TextSize(g_fonts.tiny, b).x, y + S(10.0f)), C(p.text, 0.6f * alpha), b);
    }
    y += S(58.0f);
    const float savedFade = g_fade;
    g_fade *= alpha;
    if (RoundButton(d, "##prev", ImVec2(x + S(24.0f), y), S(24.0f), icons::kSkipBackFilled, S(18.0f), false, t.canPrevious)) media::Previous();
    {
        const ImVec2 c(x + S(24.0f) + S(72.0f), y);
        if (t.playing) d->AddCircleFilled(c, S(32.0f) + S(12.0f) * g_bassSmooth, C(p.accent, 0.20f + 0.25f * g_bassSmooth), 48);
        if (RoundButton(d, "##playpause", c, S(32.0f), g_playMorph > 0.5f ? icons::kPlayFilled : icons::kPauseFilled, S(22.0f), true,
                        t.canPlayPause)) {
            media::PlayPause();
        }
    }
    if (RoundButton(d, "##next", ImVec2(x + S(24.0f) + S(144.0f), y), S(24.0f), icons::kSkipForwardFilled, S(18.0f), false, t.canNext)) {
        media::Next();
    }
    g_fade = savedFade;
}

// The dock: the sections, in a glass pill at the bottom.
void DrawDock(ImDrawList* d, ImVec2 display) {
    const Palette& p = P();
    struct Item {
        Page page;
        const char* icon;
        const char* key;
    };
    const Item items[] = {
        {Page::Home, kIconHome, "nav.home"},
        {Page::Lyrics, icons::kList, "nav.lyrics"},
        {Page::Spotify, icons::kMusic2, "nav.spotify"},
        {Page::Settings, kIconSliders, "nav.settings"},
    };
    constexpr int kCount = 4;
    float widths[kCount];
    float total = S(12.0f);
    for (int i = 0; i < kCount; ++i) {
        widths[i] = TextSize(g_fonts.semibold, T(items[i].key)).x + S(58.0f);
        total += widths[i];
    }
    const float h = S(kDockH);
    const ImVec2 min((display.x - total) * 0.5f, display.y - S(kDockBottom) - h);
    const ImVec2 max(min.x + total, min.y + h);
    // glass
    d->AddRectFilled(ImVec2(min.x, min.y + S(8.0f)), ImVec2(max.x, max.y + S(10.0f)), Col(ImVec4(0, 0, 0, 1), 0.22f), h * 0.5f);
    d->AddRectFilled(min, max, Col(Mix(p.card, ImVec4(1, 1, 1, 1), 0.06f), 0.55f + 0.30f * (1.0f - g_glass)), h * 0.5f);
    d->AddRect(min, max, Col(ImVec4(1, 1, 1, 1), 0.14f), h * 0.5f, 0, 1.0f);
    // which one is lit (it slides)
    int active = 0;
    for (int i = 0; i < kCount; ++i) {
        if (items[i].page == g_page) active = i;
    }
    g_dockLit += (static_cast<float>(active) - g_dockLit) * std::min(1.0f, Dt() * 12.0f);
    auto itemX = [&](float index) {
        float xx = min.x + S(6.0f);
        const int whole = static_cast<int>(std::floor(index));
        for (int i = 0; i < whole && i < kCount; ++i) xx += widths[i];
        const float frac = index - static_cast<float>(whole);
        if (whole < kCount - 1) xx += widths[whole] * frac;
        return xx;
    };
    {
        const int lo = std::clamp(static_cast<int>(std::floor(g_dockLit)), 0, kCount - 1);
        const int hi = std::min(lo + 1, kCount - 1);
        const float f = g_dockLit - static_cast<float>(lo);
        const float lw = widths[lo] + (widths[hi] - widths[lo]) * f;
        const float lx = itemX(g_dockLit);
        const ImVec2 lmin(lx, min.y + S(6.0f)), lmax(lx + lw, max.y - S(6.0f));
        for (int g = 3; g >= 1; --g) {
            const float e = S(3.0f) * static_cast<float>(g);
            d->AddRectFilled(ImVec2(lmin.x - e, lmin.y - e), ImVec2(lmax.x + e, lmax.y + e), Col(p.accent, 0.07f), (lmax.y - lmin.y) * 0.5f + e);
        }
        d->AddRectFilled(lmin, lmax, Col(p.accent), (lmax.y - lmin.y) * 0.5f);
    }
    float xx = min.x + S(6.0f);
    for (int i = 0; i < kCount; ++i) {
        const ImVec2 imin(xx, min.y + S(6.0f)), imax(xx + widths[i], max.y - S(6.0f));
        char id[24];
        std::snprintf(id, sizeof(id), "##dock%d", i);
        float hover = 0.0f;
        if (Hit(id, imin, imax, &hover)) GoTo(items[i].page);
        const float lit = std::clamp(1.0f - std::fabs(g_dockLit - static_cast<float>(i)), 0.0f, 1.0f);
        if (hover > 0.0f && lit < 0.5f) d->AddRectFilled(imin, imax, Col(ImVec4(1, 1, 1, 1), 0.07f * hover), (imax.y - imin.y) * 0.5f);
        const ImU32 col = Col(Mix(Mix(p.text, ImVec4(1, 1, 1, 1), 0.0f), p.onAccent, lit), 0.75f + 0.25f * std::max(hover, lit));
        icons::Draw(d, items[i].icon, ImVec2(imin.x + S(24.0f), (imin.y + imax.y) * 0.5f), S(18.0f), col, S(1.7f));
        Text(d, g_fonts.semibold, ImVec2(imin.x + S(42.0f), (imin.y + imax.y - g_fonts.semibold->FontSize) * 0.5f), col, T(items[i].key));
        xx += widths[i];
    }
}

// The PLAY orb: bottom right, breathing, beating with the music.
void DrawOrb(ImDrawList* d, ImVec2 display, const LocalData& data) {
    const Palette& p = P();
    const PlaySnapshot snap = g_flow.Snapshot();
    const bool busy = g_flow.Busy();
    const float now = static_cast<float>(ImGui::GetTime());
    const float r = S(36.0f);
    const ImVec2 c(display.x - S(74.0f), display.y - S(kDockBottom) - S(kDockH) * 0.5f - S(6.0f));
    float hover = 0.0f;
    if (Hit("##play", ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), &hover, !busy)) {
        g_flow.Start(g_upd.supportedMinecraft);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {  // (right click: the Luma folder, the logs)
        g_playMenuOpen = !g_playMenuOpen;
        g_playMenuAnchor = ImVec2(c.x - S(230.0f), c.y - r);
    }
    const float press = Anim(ImGui::GetID("##play") + 5, ImGui::IsItemActive() && !busy, 22.0f);
    const float breathe = 0.5f + 0.5f * std::sin(now * 2.0f);
    const float beat = g_bassSmooth;
    const bool loaded = data.game.lumaLoaded;
    // its light
    SoftLight(d, c, r * 3.0f, p.accent, 0.8f + 0.25f * breathe + 0.3f * beat + 0.4f * hover);
    const float rr = r * (1.0f + 0.05f * beat + 0.04f * hover - 0.06f * press);
    d->AddCircleFilled(c, rr, Col(busy ? Mix(p.accent, p.card, 0.35f) : p.accent), 64);
    d->AddCircle(c, rr - S(1.0f), Col(ImVec4(1, 1, 1, 1), 0.35f), 64, 1.0f);
    if (busy) {
        d->PathArcTo(c, rr + S(7.0f), now * 5.0f, now * 5.0f + 4.0f, 36);
        d->PathStroke(Col(p.accent), ImDrawFlags_None, S(3.0f));
        Spinner(d, c, S(11.0f), Col(p.onAccent));
    } else if (loaded) {
        icons::Draw(d, kIconCheck, c, S(26.0f), Col(p.onAccent), S(2.4f));
    } else {
        icons::Draw(d, kIconPlay, ImVec2(c.x + S(2.0f), c.y), S(28.0f), Col(p.onAccent));
    }
    const char* label = loaded ? T("orb.in_game") : T("home.play");
    const ImVec2 ls = TextSize(g_fonts.tiny, label);
    Text(d, g_fonts.tiny, ImVec2(c.x - ls.x * 0.5f, c.y + r + S(8.0f)), Col(p.text, 0.85f), label);
    // While PLAY works (or something went wrong): a glass bubble to its left.
    if (snap.state != PlayState::Idle && snap.state != PlayState::Done && snap.state != PlayState::AlreadyLoaded) {
        const float bw = S(320.0f);
        const ImVec2 bmax(c.x + r, c.y - r - S(26.0f));
        const ImVec2 bmin(bmax.x - bw, bmax.y - S(60.0f));
        d->AddRectFilled(bmin, bmax, Col(p.card, 0.85f), S(16.0f));
        d->AddRect(bmin, bmax, Col(ImVec4(1, 1, 1, 1), 0.12f), S(16.0f));
        DrawPlayStatus(d, ImVec2(bmin.x + S(14.0f), bmin.y + S(10.0f)), bw - S(28.0f), snap);
        if (snap.state == PlayState::Unsupported && !busy) {
            if (Button("##play_anyway", ImVec2(bmin.x, bmin.y - S(46.0f)), T("play.anyway"), false)) {
                g_flow.Start(g_upd.supportedMinecraft, true);
            }
        }
    }
}

// The game's state, a small glass pill at the top center (and the brand at the left).
void DrawStageTop(ImDrawList* d, ImVec2 display, const LocalData& data, const std::string& name, const account::Info& acc) {
    const Palette& p = P();
    // Luma, top left
    icons::Draw(d, icons::kLumaLogo, ImVec2(S(34.0f), S(26.0f)), S(24.0f), Col(p.text));
    Text(d, g_fonts.semibold, ImVec2(S(54.0f), S(26.0f) - g_fonts.semibold->FontSize * 0.5f), Col(p.text), "Luma");
    // the game, top center
    char text[128];
    const bool loaded = data.game.lumaLoaded;
    if (data.game.gameVersion.empty()) {
        std::snprintf(text, sizeof(text), "Minecraft  ·  %s", T("home.not_installed"));
    } else {
        std::snprintf(text, sizeof(text), "Minecraft %s  ·  %s", data.game.gameVersion.c_str(),
                      loaded ? T("home.luma_loaded") : T("home.ready_to_play"));
    }
    const ImVec2 ts = TextSize(g_fonts.tiny, text);
    const ImVec2 pmin(display.x * 0.5f - ts.x * 0.5f - S(22.0f), S(12.0f));
    const ImVec2 pmax(display.x * 0.5f + ts.x * 0.5f + S(14.0f), S(12.0f) + S(28.0f));
    d->AddRectFilled(pmin, pmax, Col(p.card, 0.45f + 0.4f * (1.0f - g_glass)), S(14.0f));
    d->AddRect(pmin, pmax, Col(ImVec4(1, 1, 1, 1), 0.10f), S(14.0f));
    Dot(d, ImVec2(pmin.x + S(14.0f), (pmin.y + pmax.y) * 0.5f), loaded ? p.success : p.accent);
    Text(d, g_fonts.tiny, ImVec2(pmin.x + S(26.0f), (pmin.y + pmax.y - ts.y) * 0.5f), Col(p.text, 0.85f), text);
    // you, top right (before the window's buttons): click = Settings
    const ImVec2 ac(display.x - S(232.0f), S(24.0f));
    float hover = 0.0f;
    const ImVec2 amin(ac.x - S(18.0f), ac.y - S(18.0f)), amax(ac.x + S(18.0f), ac.y + S(18.0f));
    if (Hit("##me", amin, amax, &hover)) GoTo(Page::Settings);
    if (g_noDragCount < 6) g_noDrag[g_noDragCount++] = {amin, amax};
    if (hover > 0.0f) d->AddCircleFilled(ac, S(21.0f), Col(ImVec4(1, 1, 1, 1), 0.10f * hover), 32);
    const float saved = g_fade;
    g_fade = 1.0f;
    Avatar(d, ac, S(15.0f), name);
    g_fade = saved;
    (void)acc;
}

void DrawComingSoon(ImDrawList* d, ImVec2 min, ImVec2 max, const char* icon, const char* titleKey, const char* textKey);  // (below)
void DrawSettings(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc, const LocalData& data);  // (below)

// The other sections: a glass sheet that rises over the stage.
void DrawSheet(ImDrawList* d, ImVec2 display, const account::Info& acc, const LocalData& data) {
    const Palette& p = P();
    if (g_page != Page::Home) g_sheetPage = g_page;
    g_sheet += ((g_page != Page::Home ? 1.0f : 0.0f) - g_sheet) * std::min(1.0f, Dt() * 9.0f);
    if (g_sheet < 0.01f) return;
    const float s = EaseOutCubic(g_sheet);
    // the stage dims a little behind it
    d->AddRectFilled(ImVec2(0, 0), display, Col(ImVec4(0, 0, 0, 1), 0.38f * s));
    const float top = S(56.0f) + (1.0f - s) * display.y * 0.35f;
    const ImVec2 min(S(48.0f), top);
    const ImVec2 max(display.x - S(48.0f), display.y - S(kDockBottom) - S(kDockH) - S(16.0f) + (1.0f - s) * display.y * 0.35f);
    d->AddRectFilled(ImVec2(min.x, min.y + S(10.0f)), ImVec2(max.x, max.y + S(14.0f)), Col(ImVec4(0, 0, 0, 1), 0.25f * s), S(26.0f));
    d->AddRectFilled(min, max, Col(p.background, (0.78f + 0.2f * (1.0f - g_glass)) * s), S(26.0f));
    d->AddRect(min, max, Col(ImVec4(1, 1, 1, 1), 0.10f * s), S(26.0f));
    d->AddLine(ImVec2(min.x + S(26.0f), min.y + 1.0f), ImVec2(max.x - S(26.0f), min.y + 1.0f), Col(ImVec4(1, 1, 1, 1), 0.14f * s));
    const float saved = g_fade;
    g_fade = s;
    const ImVec2 cmin(min.x + S(24.0f), min.y + S(24.0f)), cmax(max.x - S(24.0f), max.y - S(24.0f));
    d->PushClipRect(min, max, true);
    switch (g_sheetPage) {
        case Page::Lyrics: DrawComingSoon(d, cmin, cmax, icons::kList, "nav.lyrics", "lyrics.soon_text"); break;
        case Page::Spotify: DrawComingSoon(d, cmin, cmax, icons::kMusic2, "nav.spotify", "spotify.soon_text"); break;
        default: DrawSettings(d, cmin, cmax, acc, data); break;
    }
    d->PopClipRect();
    g_fade = saved;
}

// The whole stage (Home, with the sheet over it for the other sections).
void DrawStage(ImDrawList* d, ImVec2 display, const media::Track& t, const LocalData& data, const std::string& name, const account::Info& acc) {
    const float stageAlpha = 1.0f - 0.35f * EaseOutCubic(g_sheet);
    const ImVec2 smin(S(40.0f), S(70.0f));
    const ImVec2 smax(display.x - S(40.0f), display.y - S(kDockBottom) - S(kDockH) - S(110.0f));
    // (it comes in when the launcher opens)
    static float openIn = 0.0f;
    openIn = std::min(1.0f, openIn + Dt() * 1.4f);
    const float in = EaseOutCubic(openIn);
    const float savedFade = g_fade;
    g_fade = in;
    DrawStageSong(d, ImVec2(smin.x, smin.y + (1.0f - in) * S(20.0f)), ImVec2(smax.x, smax.y + (1.0f - in) * S(20.0f)), t, stageAlpha);
    g_fade = savedFade;
    DrawSheet(d, display, acc, data);
    DrawDock(d, display);
    DrawOrb(d, display, data);
    (void)name;
}

void DrawMusicHome(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data, const std::string& name, const media::Track& t) {
    const Palette& p = P();
    const float baseFade = g_fade;
    // 1. Greeting.
    {
        const float a = Stagger(0);
        g_fade = baseFade * a;
        const float dy = (1.0f - a) * S(10.0f);
        char hello[160];
        std::snprintf(hello, sizeof(hello), "%s, %s", T(GreetingKey()), name.c_str());
        TextFit(d, g_fonts.title, ImVec2(min.x + S(4.0f), min.y + S(6.0f) + dy), C(p.text), hello, max.x - min.x);
        Text(d, g_fonts.regular, ImVec2(min.x + S(4.0f), min.y + S(6.0f) + g_fonts.title->FontSize + S(4.0f) + dy), C(p.textDim),
             T("home.music_tagline"));
    }
    const float top = min.y + S(70.0f);
    const float barH = S(88.0f);
    // 2. What's playing.
    {
        const float a = Stagger(1);
        g_fade = baseFade * a;
        const float dy = (1.0f - a) * S(16.0f);
        DrawNowPlaying(d, ImVec2(min.x, top + dy), ImVec2(max.x, max.y - barH - S(kGap) + dy), t);
    }
    // 3. Play.
    {
        const float a = Stagger(2);
        g_fade = baseFade * a;
        const float dy = (1.0f - a) * S(16.0f);
        DrawPlayBar(d, ImVec2(min.x, max.y - barH + dy), ImVec2(max.x, max.y + dy), data);
    }
    g_fade = baseFade;
}

void DrawHome(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data, const std::string& name) {
    const float width = max.x - min.x;
    const float height = max.y - min.y;
    const float gap = S(kGap);
    const float heroH = std::clamp(height * 0.42f, S(250.0f), S(380.0f));
    const float rest = height - heroH - gap * 2.0f;
    const float row1H = std::clamp(rest * 0.48f, S(150.0f), S(240.0f));
    const float row2H = rest - row1H;
    DrawHero(d, min, ImVec2(max.x, min.y + heroH), data, name);
    float y = min.y + heroH + gap;
    // Fila 1: acceso rápido | tu skin | estadísticas
    const float qaW = (width - gap * 2.0f) * 0.42f;
    const float skinW = (width - gap * 2.0f) * 0.24f;
    DrawQuickAccess(d, ImVec2(min.x, y), ImVec2(min.x + qaW, y + row1H));
    DrawSkinCard(d, ImVec2(min.x + qaW + gap, y), ImVec2(min.x + qaW + gap + skinW, y + row1H), data);
    DrawStats(d, ImVec2(min.x + qaW + gap + skinW + gap, y), ImVec2(max.x, y + row1H), data);
    y += row1H + gap;
    // Fila 2: noticias | info del cliente
    const float newsW = (width - gap) * 0.62f;
    DrawNews(d, ImVec2(min.x, y), ImVec2(min.x + newsW, y + row2H));
    DrawClientInfo(d, ImVec2(min.x + newsW + gap, y), ImVec2(max.x, y + row2H), data.game);
}

// --- Ajustes --------------------------------------------------------------------------------
// Una fila de ajustes: título, explicación opcional debajo, y el control a la derecha.
float SettingLabel(ImDrawList* d, ImVec2 pos, float width, const char* title, const char* hint) {
    const Palette& p = P();
    Text(d, g_fonts.semibold, pos, C(p.text), title);
    float h = g_fonts.semibold->FontSize;
    if (hint != nullptr) {
        h += S(3.0f) + TextWrap(d, g_fonts.tiny, ImVec2(pos.x, pos.y + h + S(3.0f)), C(p.textDim), hint, width);
    }
    return h;
}

void DrawSettingsGeneral(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("settings.general"));
    const float x = min.x + S(22.0f);
    const float right = max.x - S(22.0f);
    float y = min.y + S(62.0f);
    const float rowGap = S(22.0f);

    // Idioma (los nombres de los idiomas siempre en su propio idioma)
    {
        const char* labels[3] = {i18n::LanguageName(i18n::Language::English), i18n::LanguageName(i18n::Language::Spanish),
                                 i18n::LanguageName(i18n::Language::Portuguese)};
        int index = static_cast<int>(i18n::CurrentLanguage());
        const float w = SegmentedWidth(labels, 3);
        const float labelW = right - x - w - S(20.0f);
        const float h = SettingLabel(d, ImVec2(x, y), labelW, T("settings.language"), T("settings.lang_hint"));
        if (Segmented("##lang", ImVec2(right - w, y), labels, 3, &index)) {
            i18n::SetLanguage(static_cast<i18n::Language>(index));
            SaveConfig();
        }
        y += std::max(h, S(38.0f)) + rowGap;
    }
    d->AddLine(ImVec2(x, y - rowGap * 0.5f), ImVec2(right, y - rowGap * 0.5f), C(p.border));
    // Tema
    {
        const char* labels[2] = {T("settings.dark"), T("settings.light")};
        int index = CurrentTheme() == ThemeKind::Light ? 1 : 0;
        const float w = SegmentedWidth(labels, 2);
        SettingLabel(d, ImVec2(x, y + S(9.0f)), right - x - w, T("settings.theme"), nullptr);
        if (Segmented("##theme", ImVec2(right - w, y), labels, 2, &index)) {
            SetTheme(index == 1 ? ThemeKind::Light : ThemeKind::Dark);
            SaveConfig();
        }
        y += S(38.0f) + rowGap;
    }
    d->AddLine(ImVec2(x, y - rowGap * 0.5f), ImVec2(right, y - rowGap * 0.5f), C(p.border));
    // Cielo del inicio
    {
        const char* labels[4] = {T("sky.auto"), T("sky.day"), T("sky.sunset"), T("sky.night")};
        const float w = SegmentedWidth(labels, 4);
        int index = static_cast<int>(g_skyMode);
        SettingLabel(d, ImVec2(x, y + S(9.0f)), right - x - w, T("settings.sky"), nullptr);
        if (Segmented("##sky", ImVec2(right - w, y), labels, 4, &index)) {
            g_skyMode = static_cast<sky::Mode>(index);
            SaveConfig();
        }
        y += S(38.0f) + rowGap;
    }
    d->AddLine(ImVec2(x, y - rowGap * 0.5f), ImVec2(right, y - rowGap * 0.5f), C(p.border));
    // Después de JUGAR
    {
        const char* labels[2] = {T("settings.minimize"), T("settings.stay")};
        int index = g_minimizeAfterPlay ? 0 : 1;
        const float w = SegmentedWidth(labels, 2);
        SettingLabel(d, ImVec2(x, y + S(9.0f)), right - x - w, T("settings.after_play"), nullptr);
        if (Segmented("##after", ImVec2(right - w, y), labels, 2, &index)) {
            g_minimizeAfterPlay = index == 0;
            SaveConfig();
        }
        y += S(38.0f) + rowGap;
    }
    d->AddLine(ImVec2(x, y - rowGap * 0.5f), ImVec2(right, y - rowGap * 0.5f), C(p.border));
    // Abrir con Windows
    {
        const float h = SettingLabel(d, ImVec2(x, y), right - x - S(70.0f), T("settings.startup"), T("settings.startup_hint"));
        if (Toggle("##startup", ImVec2(right - S(46.0f), y + std::max(0.0f, (h - S(26.0f)) * 0.5f)), &g_startWithWindows)) {
            WriteStartup(g_startWithWindows);
            g_startWithWindows = ReadStartup();  // lo que quedó de verdad en el registro
        }
    }
}

void DrawSettingsFolders(ImDrawList* d, ImVec2 min, ImVec2 max) {
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("settings.folders"));
    struct Folder {
        const char* id;
        const char* key;
        const wchar_t* sub;
    };
    const Folder folders[] = {
        {"##f_logs", "settings.logs", L"logs"},
        {"##f_config", "settings.config", L"config"},
        {"##f_bench", "settings.benchmarks", L"benchmarks"},
        {"##f_data", "settings.data", L""},
    };
    float x = min.x + S(22.0f);
    float y = min.y + S(58.0f);
    for (const Folder& f : folders) {
        const float w = TextSize(g_fonts.semibold, T(f.key)).x + S(32.0f);
        if (x + w > max.x - S(22.0f)) {
            x = min.x + S(22.0f);
            y += S(46.0f);
        }
        if (Button(f.id, ImVec2(x, y), T(f.key), false)) {
            OpenFolder(f.sub[0] != 0 ? DataDir() / f.sub : DataDir());
        }
        x += w + S(10.0f);
    }
}

void DrawSettingsAccount(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc, const LocalData& data) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("settings.account"));
    const float x = min.x + S(22.0f);
    const float w = (max.x - min.x) - S(44.0f);
    float y = min.y + S(60.0f);
    const std::string name = PlayerName(acc, data);
    Avatar(d, ImVec2(x + S(28.0f), y + S(28.0f)), S(28.0f), name);
    TextFit(d, g_fonts.title, ImVec2(x + S(70.0f), y + S(4.0f)), C(p.text), name.c_str(), w - S(70.0f));
    const bool signedIn = acc.state == account::State::SignedIn;
    const char* status = signedIn ? T("account.signed_in") : T("nav.not_signed_in");
    TextFit(d, g_fonts.tiny, ImVec2(x + S(70.0f), y + S(34.0f)), C(p.textDim), status, w - S(70.0f));
    y += S(74.0f);

    const bool busy = acc.state == account::State::SigningIn || acc.state == account::State::Refreshing;
    if (busy) {
        Spinner(d, ImVec2(x + S(8.0f), y + S(9.0f)), S(6.5f), C(p.warning));
        TextFit(d, g_fonts.regular, ImVec2(x + S(24.0f), y), C(p.text),
                acc.state == account::State::SigningIn ? T("account.waiting") : T("account.refreshing"), w - S(24.0f));
        y += S(30.0f);
        if (acc.state == account::State::SigningIn && Button("##acc_cancel", ImVec2(x, y), T("account.cancel"), false)) {
            account::Cancel();
        }
        return;
    }
    if (acc.state == account::State::Error && !acc.errorKey.empty()) {
        icons::Draw(d, kIconAlert, ImVec2(x + S(8.0f), y + S(9.0f)), S(16.0f), C(p.danger), S(1.6f));
        const float h = TextWrap(d, g_fonts.regular, ImVec2(x + S(24.0f), y), C(p.danger), T(acc.errorKey.c_str()), w - S(24.0f));
        y += h + S(4.0f);
        if (!acc.detail.empty()) {
            TextFit(d, g_fonts.tiny, ImVec2(x + S(24.0f), y), C(p.textDim), acc.detail.c_str(), w - S(24.0f));
            y += g_fonts.tiny->FontSize + S(4.0f);
        }
        y += S(8.0f);
    }
    if (signedIn) {
        if (Button("##acc_out", ImVec2(x, y), T("account.sign_out"), false)) {
            account::SignOut();
        }
    } else {
        const float h = TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("account.why"), w);
        y += h + S(12.0f);
        if (Button("##acc_in", ImVec2(x, y), T("account.sign_in"), true)) {
            account::SignIn();
        }
    }
}

void DrawSettingsAbout(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("settings.about"));
    const float x = min.x + S(22.0f);
    const float right = max.x - S(22.0f);
    const float w = right - x;
    float y = min.y + S(58.0f);
    icons::Draw(d, icons::kLumaLogo, ImVec2(x + S(18.0f), y + S(16.0f)), S(34.0f), C(p.text));
    char version[96];
    std::snprintf(version, sizeof(version), T("settings.version"), kVersion);
    if (!updater::OfficialBuild()) {
        std::snprintf(version + std::strlen(version), sizeof(version) - std::strlen(version), "  ·  %s", T("update.dev_build"));
    }
    Text(d, g_fonts.semibold, ImVec2(x + S(50.0f), y), C(p.text), "Luma");
    Text(d, g_fonts.tiny, ImVec2(x + S(50.0f), y + g_fonts.semibold->FontSize + S(2.0f)), C(p.textDim), version);
    y += S(46.0f);
    y += TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("settings.about_text"), w) + S(16.0f);

    // Actualizaciones: estado, "buscar ahora" / "actualizar ahora" y la opción automática.
    d->AddLine(ImVec2(x, y - S(8.0f)), ImVec2(right, y - S(8.0f)), C(p.border));
    char status[160];
    const ImVec4* color = &p.textDim;
    switch (g_upd.state) {
        case updater::State::Checking: std::snprintf(status, sizeof(status), "%s", T("update.checking")); break;
        case updater::State::UpToDate:
            std::snprintf(status, sizeof(status), "%s", T("update.up_to_date"));
            color = &p.success;
            break;
        case updater::State::Available:
            std::snprintf(status, sizeof(status), T("update.available"), g_upd.latestVersion.c_str());
            color = &p.warning;
            break;
        case updater::State::Failed:
            std::snprintf(status, sizeof(status), "%s", T(g_upd.errorKey.empty() ? "update.failed" : g_upd.errorKey.c_str()));
            color = &p.danger;
            break;
        case updater::State::Offline: std::snprintf(status, sizeof(status), "%s", T("update.offline")); break;
        default: std::snprintf(status, sizeof(status), "%s", T("settings.updates")); break;
    }
    if (g_upd.state == updater::State::Checking) {
        Spinner(d, ImVec2(x + S(7.0f), y + S(9.0f)), S(5.5f), C(p.textDim));
    } else {
        Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), *color);
    }
    TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), status, w - S(20.0f));
    y += S(30.0f);
    const bool canInstall = g_upd.state == updater::State::Available && updater::OfficialBuild();
    if (canInstall) {
        if (Button("##upd_install", ImVec2(x, y), T("update.install_now"), true)) {
            updater::InstallNow();
        }
    } else if (Button("##upd_check", ImVec2(x, y), T("update.check_now"), false, nullptr,
                      g_upd.state != updater::State::Checking)) {
        updater::CheckNow();
    }
    if (Toggle("##auto_update", ImVec2(right - S(46.0f), y + S(6.0f)), &g_autoUpdate)) {
        updater::SetAutoInstall(g_autoUpdate);
        SaveConfig();
    }
    const char* autoLabel = T("update.auto");
    const ImVec2 als = TextSize(g_fonts.tiny, autoLabel);
    Text(d, g_fonts.tiny, ImVec2(right - S(56.0f) - als.x, y + S(10.0f)), C(p.textDim), autoLabel);
}

void DrawSettings(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc, const LocalData& data) {
    // Cada tarjeta mide lo que necesita su contenido (no se estiran hasta abajo).
    const float gap = S(kGap);
    const float leftW = (max.x - min.x) * 0.56f;
    const float generalH = S(392.0f);
    const float foldersH = S(126.0f);
    DrawSettingsGeneral(d, min, ImVec2(min.x + leftW, min.y + generalH));
    DrawSettingsFolders(d, ImVec2(min.x, min.y + generalH + gap), ImVec2(min.x + leftW, min.y + generalH + gap + foldersH));
    const float rightX = min.x + leftW + gap;
    const float accountH = S(220.0f);
    DrawSettingsAccount(d, ImVec2(rightX, min.y), ImVec2(max.x, min.y + accountH), acc, data);
    DrawSettingsAbout(d, ImVec2(rightX, min.y + accountH + gap), ImVec2(max.x, min.y + generalH + gap + foldersH));
    const float rowY = min.y + generalH + gap + foldersH + gap;
    const float rowH = std::min(S(170.0f), max.y - rowY);
    if (rowH > S(120.0f)) {
        DrawSettingsShortcuts(d, ImVec2(min.x, rowY), ImVec2(min.x + leftW, rowY + rowH));
        DrawSettingsLinks(d, ImVec2(rightX, rowY), ImVec2(max.x, rowY + rowH));
    }
}

// Pantalla de "Actualizando Luma..." (tapa todo mientras baja e instala).
void DrawUpdateOverlay(ImDrawList* d, ImVec2 display) {
    const Palette& p = P();
    d->AddRectFilled(ImVec2(0, 0), display, Col(p.background, 0.92f));
    // Atrapar el mouse: mientras se actualiza no se puede tocar nada.
    ImGui::SetCursorScreenPos(ImVec2(0, S(kTitleBarHeight)));
    ImGui::InvisibleButton("##update_block", ImVec2(display.x, std::max(1.0f, display.y - S(kTitleBarHeight))));
    const ImVec2 size(S(420.0f), S(190.0f));
    const ImVec2 min((display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    d->AddRectFilled(min, max, Col(p.card), S(18.0f));
    d->AddRect(min, max, Col(p.border), S(18.0f));
    icons::Draw(d, icons::kLumaLogo, ImVec2((min.x + max.x) * 0.5f, min.y + S(46.0f)), S(44.0f), Col(p.text));
    char title[96];
    std::snprintf(title, sizeof(title), T("update.downloading"), g_upd.latestVersion.c_str());
    const char* text = g_upd.state == updater::State::Downloading ? title : T("update.installing");
    const ImVec2 ts = TextSize(g_fonts.semibold, text);
    Text(d, g_fonts.semibold, ImVec2((min.x + max.x - ts.x) * 0.5f, min.y + S(88.0f)), Col(p.text), text);
    // Barra de progreso
    const ImVec2 bmin(min.x + S(40.0f), min.y + S(130.0f));
    const ImVec2 bmax(max.x - S(40.0f), bmin.y + S(8.0f));
    d->AddRectFilled(bmin, bmax, Col(p.background), S(4.0f));
    const float progress = g_upd.state == updater::State::Downloading ? g_upd.progress : 1.0f;
    if (progress > 0.0f) {
        d->AddRectFilled(bmin, ImVec2(bmin.x + (bmax.x - bmin.x) * std::clamp(progress, 0.0f, 1.0f), bmax.y), Col(p.accent),
                         S(4.0f));
    }
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(progress * 100.0f + 0.5f));
    const ImVec2 pts = TextSize(g_fonts.tiny, percent);
    Text(d, g_fonts.tiny, ImVec2((min.x + max.x - pts.x) * 0.5f, bmax.y + S(10.0f)), Col(p.textDim), percent);
}

// --- Sección Skins: "vitrina + guardarropa" -------------------------------------------
//  Arriba: título, cuántas skins hay, filtro e Importar.
//  Izquierda (vitrina): la skin elegida en grande sobre el cielo, con su nombre;
//    abajo, una franja con el nombre, etiquetas y las acciones (Equipar es la
//    principal; favorita, carpeta y borrar son botones de ícono).
//  Derecha (guardarropa): tarjetas iguales, una por skin.
std::string g_selectedSkin;          // id de la skin elegida en la sección
std::string g_currentGameSkin;       // id de la skin que usas ahora en el juego
std::wstring g_lastSyncedKey;        // para agregar a la biblioteca solo cuando cambia skin.png
int g_skinFilter = 0;                // 0 = todas, 1 = favoritas
float g_skinScroll = 0.0f;
float g_skinsYaw = -0.45f;
double g_deleteArmedAt = -10.0;      // borrar pide un segundo clic (dentro de 3 s)
std::string g_importError;

constexpr const char* kIconStarLine = icons::kStar;
constexpr const char* kIconTrash = icons::kTrash;
constexpr const char* kIconPlus = icons::kPlus;
constexpr const char* kIconGamepad = icons::kGamepad;

// Si Luma.dll guardó una skin nueva, sumarla a la biblioteca (cada frame, barato).
void SyncSkins(const LocalData& data) {
    if (data.skinKey.empty() || data.skinKey == g_lastSyncedKey) return;
    g_lastSyncedKey = data.skinKey;
    g_currentGameSkin = skins::SyncFromGame(data.skinPath);
    if (g_selectedSkin.empty()) g_selectedSkin = g_currentGameSkin;
}

const skins::Skin* SelectedSkin() {
    const auto& list = skins::List();
    for (const skins::Skin& s : list) {
        if (s.id == g_selectedSkin) return &s;
    }
    return list.empty() ? nullptr : &list.front();
}

bool LibraryModel(const skins::Skin& s, skin3d::Model& model) {
    const textures::Texture* tex = textures::Get(s.file);
    if (tex == nullptr) return false;
    model.texture = tex->id;
    model.slim = s.slim;
    model.legacy = s.legacy;
    return true;
}

// Botón cuadrado con un ícono (sobre la vitrina: vidrio blanco).
bool IconButton(const char* id, ImVec2 pos, float size, const char* icon, bool active, const ImVec4& activeColor,
                const char* tooltip) {
    float hover = 0.0f;
    const ImVec2 max(pos.x + size, pos.y + size);
    const bool clicked = Hit(id, pos, max, &hover);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(pos, max, C(ImVec4(1, 1, 1, 1), 0.10f + 0.10f * hover), S(10.0f));
    d->AddRect(pos, max, C(ImVec4(1, 1, 1, 1), 0.16f), S(10.0f));
    icons::Draw(d, icon, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.5f,
                C(active ? activeColor : ImVec4(1, 1, 1, 0.9f)), S(1.7f));
    if (ImGui::IsItemHovered() && tooltip != nullptr) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

// Una etiqueta chiquita ("Del juego", "Brazos finos").
float Chip(ImDrawList* d, ImVec2 pos, const char* text) {
    const ImVec2 ts = TextSize(g_fonts.tiny, text);
    const ImVec2 max(pos.x + ts.x + S(16.0f), pos.y + ts.y + S(8.0f));
    d->AddRectFilled(pos, max, C(ImVec4(1, 1, 1, 1), 0.14f), (max.y - pos.y) * 0.5f);
    Text(d, g_fonts.tiny, ImVec2(pos.x + S(8.0f), pos.y + S(4.0f)), C(ImVec4(1, 1, 1, 1), 0.92f), text);
    return max.x - pos.x;
}

void DrawShowcase(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    const float rounding = S(kRound + 4.0f);
    const skins::Skin* s = SelectedSkin();
    sky::Draw(d, min, max, S(5.0f), rounding, C(p.background), g_skyMode);
    d->AddRect(min, max, C(p.border), rounding);
    if (s == nullptr) {
        const char* text = T("skins.empty");
        const float w = max.x - min.x - S(60.0f);
        TextWrap(d, g_fonts.semibold, ImVec2(min.x + S(30.0f), (min.y + max.y) * 0.5f - S(20.0f)), C(ImVec4(1, 1, 1, 1)), text, w);
        return;
    }
    // La franja de abajo (oscurece el cielo para que se lea).
    const float panelH = S(150.0f);
    const float panelY = max.y - panelH;
    // El personaje, arriba de la franja (se gira arrastrando).
    const ImVec2 stageMin(min.x, min.y);
    const ImVec2 stageMax(max.x, panelY + S(30.0f));
    DragToRotate("##showcase", ImVec2(min.x, min.y), ImVec2(max.x, panelY), g_skinsYaw);
    skin3d::Model model;
    if (LibraryModel(*s, model)) {
        skin3d::View view;
        view.yaw = g_skinsYaw;
        view.time = static_cast<float>(ImGui::GetTime());
        view.alpha = g_fade;
        const float h = (stageMax.y - stageMin.y) * 0.78f;
        const ImVec2 c((stageMin.x + stageMax.x) * 0.5f, (stageMin.y + stageMax.y) * 0.5f + S(14.0f));
        ImGui::PushClipRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), true);
        const ImVec2 head = skin3d::Draw(d, ImVec2(c.x - h * 0.4f, c.y - h * 0.5f), ImVec2(c.x + h * 0.4f, c.y + h * 0.5f),
                                         model, view);
        ImGui::PopClipRect();
        // el nombre sobre la cabeza, como en el juego
        const ImVec2 ns = TextSize(g_fonts.pixel, s->name.c_str());
        const ImVec2 tmin(std::round(head.x - ns.x * 0.5f - S(10.0f)), std::round(head.y - ns.y - S(16.0f)));
        const ImVec2 tmax(std::round(head.x + ns.x * 0.5f + S(10.0f)), std::round(head.y - S(4.0f)));
        d->AddRectFilled(tmin, tmax, C(ImVec4(0, 0, 0, 1), 0.42f));
        Text(d, g_fonts.pixel, ImVec2(tmin.x + S(10.0f), tmin.y + (tmax.y - tmin.y - ns.y) * 0.5f), C(ImVec4(1, 1, 1, 1)),
             s->name.c_str());
    }
    // Franja: degradado oscuro + datos + acciones.
    d->AddRectFilledMultiColor(ImVec2(min.x + 1, panelY - S(60.0f)), ImVec2(max.x - 1, max.y - rounding),
                               C(ImVec4(0, 0, 0, 1), 0.0f), C(ImVec4(0, 0, 0, 1), 0.0f), C(ImVec4(0.03f, 0.04f, 0.08f, 1), 0.82f),
                               C(ImVec4(0.03f, 0.04f, 0.08f, 1), 0.82f));
    d->AddRectFilled(ImVec2(min.x + 1, max.y - rounding), ImVec2(max.x - 1, max.y - 1), C(ImVec4(0.03f, 0.04f, 0.08f, 1), 0.82f),
                     rounding, ImDrawFlags_RoundCornersBottom);
    const float x = min.x + S(24.0f);
    const float w = max.x - min.x - S(48.0f);
    float y = panelY + S(10.0f);
    TextFit(d, g_fonts.title, ImVec2(x, y), C(ImVec4(1, 1, 1, 1)), s->name.c_str(), w);
    y += g_fonts.title->FontSize + S(8.0f);
    float cx = x;
    cx += Chip(d, ImVec2(cx, y), s->fromGame ? T("skins.from_game") : T("skins.imported")) + S(6.0f);
    cx += Chip(d, ImVec2(cx, y), s->legacy ? T("skins.legacy") : (s->slim ? T("skins.slim") : T("skins.classic"))) + S(6.0f);
    if (s->id == g_currentGameSkin) Chip(d, ImVec2(cx, y), T("skins.current"));
    y += S(36.0f);
    // Acciones
    const bool equipped = skins::Active() == s->id;
    const float bh = S(44.0f);
    const float iconsW = bh * 3.0f + S(8.0f) * 2.0f;
    const ImVec2 emin(x, y);
    const ImVec2 emax(x + w - iconsW - S(10.0f), y + bh);
    float hover = 0.0f;
    if (Hit("##sc_equip", emin, emax, &hover, !equipped)) {
        skins::SetActive(s->id);
    }
    d->AddRectFilled(emin, emax, C(equipped ? ImVec4(1, 1, 1, 0.14f) : p.accent), S(12.0f));
    d->AddRectFilled(emin, emax, C(p.onAccent, 0.08f * hover), S(12.0f));
    const char* label = equipped ? T("closet.equipped") : T("closet.equip");
    const ImVec2 ls = TextSize(g_fonts.title, label);
    const float contentW = ls.x + (equipped ? S(26.0f) : 0.0f);
    const float lx = (emin.x + emax.x - contentW) * 0.5f;
    if (equipped) icons::Draw(d, kIconCheck, ImVec2(lx + S(9.0f), (emin.y + emax.y) * 0.5f), S(18.0f), C(ImVec4(1, 1, 1, 1)), S(2.2f));
    Text(d, g_fonts.title, ImVec2(lx + (equipped ? S(26.0f) : 0.0f), (emin.y + emax.y - ls.y) * 0.5f),
         C(equipped ? ImVec4(1, 1, 1, 1) : p.onAccent), label);
    if (ImGui::IsItemHovered() || (equipped && ImGui::IsMouseHoveringRect(emin, emax))) ImGui::SetTooltip("%s", T("closet.equip_hint"));
    float bx = emax.x + S(10.0f);
    if (IconButton("##sc_fav", ImVec2(bx, y), bh, s->favorite ? kIconStar : kIconStarLine, s->favorite, p.warning,
                   s->favorite ? T("skins.unfavorite") : T("skins.favorite"))) {
        skins::SetFavorite(s->id, !s->favorite);
    }
    bx += bh + S(8.0f);
    if (IconButton("##sc_folder", ImVec2(bx, y), bh, kIconFolder, false, p.text, T("skins.open_folder"))) {
        OpenFolder(skins::Folder());
    }
    bx += bh + S(8.0f);
    const bool armed = ImGui::GetTime() - g_deleteArmedAt < 3.0;
    if (IconButton("##sc_delete", ImVec2(bx, y), bh, kIconTrash, armed, p.danger, armed ? T("skins.confirm_delete") : T("skins.delete"))) {
        if (armed) {
            const std::string id = s->id;
            g_deleteArmedAt = -10.0;
            g_selectedSkin.clear();
            skins::Remove(id);
            return;
        }
        g_deleteArmedAt = ImGui::GetTime();
    }
}

void DrawWardrobe(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    std::vector<const skins::Skin*> shown;
    for (const skins::Skin& s : skins::List()) {
        if (g_skinFilter == 0 || s.favorite) shown.push_back(&s);
    }
    if (shown.empty()) {
        Card(d, min, max);
        const char* text = g_skinFilter == 1 ? T("skins.no_favorites") : T("skins.empty");
        const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        d->AddCircleFilled(ImVec2(c.x, c.y - S(30.0f)), S(34.0f), C(p.background));
        icons::Draw(d, g_skinFilter == 1 ? kIconStarLine : kIconSkins, ImVec2(c.x, c.y - S(30.0f)), S(32.0f), C(p.textDim), S(1.8f));
        const float w = std::min(max.x - min.x - S(60.0f), S(360.0f));
        const ImVec2 ts = g_fonts.regular->CalcTextSizeA(g_fonts.regular->FontSize, FLT_MAX, w, text);
        TextWrap(d, g_fonts.regular, ImVec2(c.x - ts.x * 0.5f, c.y + S(16.0f)), C(p.textDim), text, w);
        return;
    }
    const float gap = S(14.0f);
    const int columns = std::max(2, static_cast<int>((max.x - min.x + gap) / (S(170.0f) + gap)));
    const float cardW = ((max.x - min.x) - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float cardH = cardW * 1.32f;
    const int rows = (static_cast<int>(shown.size()) + columns - 1) / columns;
    const float contentH = rows * cardH + (rows - 1) * gap;
    const float maxScroll = std::max(0.0f, contentH - (max.y - min.y));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y) {
        g_skinScroll -= ImGui::GetIO().MouseWheel * S(80.0f);
    }
    g_skinScroll = std::clamp(g_skinScroll, 0.0f, maxScroll);
    const std::string selected = SelectedSkin() ? SelectedSkin()->id : std::string();
    const std::string active = skins::Active();
    const float t = static_cast<float>(ImGui::GetTime());
    ImGui::PushClipRect(ImVec2(min.x - S(4.0f), min.y - S(4.0f)), ImVec2(max.x + S(4.0f), max.y + S(4.0f)), true);
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const skins::Skin& s = *shown[i];
        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        char id[48];
        std::snprintf(id, sizeof(id), "##card_%s", s.id.substr(0, 12).c_str());
        const float lift = Anim(ImGui::GetID(id) + 5, ImGui::IsMouseHoveringRect(
                                    ImVec2(min.x + col * (cardW + gap), min.y + row * (cardH + gap) - g_skinScroll),
                                    ImVec2(min.x + col * (cardW + gap) + cardW, min.y + row * (cardH + gap) - g_skinScroll + cardH)),
                                12.0f);
        const ImVec2 cmin(min.x + col * (cardW + gap), min.y + row * (cardH + gap) - g_skinScroll - S(3.0f) * lift);
        const ImVec2 cmax(cmin.x + cardW, cmin.y + cardH);
        if (cmax.y < min.y || cmin.y > max.y) continue;
        float hover = 0.0f;
        if (Hit(id, cmin, cmax, &hover)) {
            g_selectedSkin = s.id;
            g_deleteArmedAt = -10.0;
        }
        const bool isSelected = s.id == selected;
        // tarjeta: arriba un degradado (el "escenario"), abajo el nombre
        const float footerH = S(46.0f);
        d->AddRectFilled(ImVec2(cmin.x + S(2), cmin.y + S(5)), ImVec2(cmax.x - S(2), cmax.y + S(5)),
                         C(ImVec4(0, 0, 0, 1), 0.10f + 0.12f * lift), S(14.0f));
        GradientRect(d, cmin, cmax, C(Mix(p.heroTop, p.cardHover, 0.3f * hover)), C(p.card), S(14.0f));
        d->AddRect(cmin, cmax, C(isSelected ? p.accent : p.border), S(14.0f), 0, isSelected ? S(2.0f) : 1.0f);
        skin3d::Model model;
        if (LibraryModel(s, model)) {
            skin3d::View view;
            view.yaw = -0.45f + std::sin(t * 1.4f) * 0.5f * lift;
            view.time = t;
            view.alpha = g_fade;
            skin3d::Draw(d, ImVec2(cmin.x + S(12.0f), cmin.y + S(16.0f)), ImVec2(cmax.x - S(12.0f), cmax.y - footerH - S(4.0f)),
                         model, view);
        }
        // pie: nombre + íconos de estado
        const float fy = cmax.y - footerH;
        d->AddLine(ImVec2(cmin.x + S(12.0f), fy), ImVec2(cmax.x - S(12.0f), fy), C(p.border));
        float badgesW = 0.0f;
        const float by = fy + footerH * 0.5f;
        float bx = cmax.x - S(16.0f);
        if (s.favorite) {
            icons::Draw(d, kIconStar, ImVec2(bx, by), S(15.0f), C(p.warning));
            bx -= S(22.0f);
            badgesW += S(22.0f);
        }
        if (s.id == g_currentGameSkin) {
            icons::Draw(d, kIconGamepad, ImVec2(bx, by), S(17.0f), C(p.success), S(1.5f));
            if (ImGui::IsMouseHoveringRect(ImVec2(bx - S(9), by - S(9)), ImVec2(bx + S(9), by + S(9)))) ImGui::SetTooltip("%s", T("skins.current"));
            bx -= S(22.0f);
            badgesW += S(22.0f);
        }
        if (s.id == active) {
            d->AddCircleFilled(ImVec2(bx, by), S(9.0f), C(p.accent));
            icons::Draw(d, kIconCheck, ImVec2(bx, by), S(12.0f), C(p.onAccent), S(2.2f));
            badgesW += S(22.0f);
        }
        TextFit(d, g_fonts.semibold, ImVec2(cmin.x + S(14.0f), by - g_fonts.semibold->FontSize * 0.5f), C(p.text), s.name.c_str(),
                cardW - S(28.0f) - badgesW);
    }
    ImGui::PopClipRect();
    if (maxScroll > 0.0f) {
        const float viewH = max.y - min.y;
        const float barH = std::max(S(30.0f), viewH * viewH / contentH);
        const float barY = min.y + (viewH - barH) * (g_skinScroll / maxScroll);
        d->AddRectFilled(ImVec2(max.x + S(6.0f), barY), ImVec2(max.x + S(10.0f), barY + barH), C(p.textDim, 0.4f), S(2.0f));
    }
}

void DrawSkinsPage(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    // Encabezado
    Text(d, g_fonts.title, ImVec2(min.x + S(4.0f), min.y + S(2.0f)), C(p.text), T("nav.skins"));
    char count[48];
    std::snprintf(count, sizeof(count), T("skins.count"), static_cast<int>(skins::List().size()));
    Text(d, g_fonts.tiny, ImVec2(min.x + S(4.0f), min.y + S(6.0f) + g_fonts.title->FontSize), C(p.textDim), count);
    const char* importLabel = T("skins.import");
    const float importW = TextSize(g_fonts.semibold, importLabel).x + S(32.0f);
    if (Button("##skins_import", ImVec2(max.x - importW, min.y + S(4.0f)), importLabel, true)) {
        const std::string id = skins::ImportWithDialog(g_hwnd, g_importError);
        if (!id.empty()) {
            g_selectedSkin = id;
            g_skinFilter = 0;
        }
    }
    const char* filters[2] = {T("skins.all"), T("skins.favorites")};
    const float fw = SegmentedWidth(filters, 2);
    Segmented("##skins_filter", ImVec2(max.x - importW - S(12.0f) - fw, min.y + S(4.0f)), filters, 2, &g_skinFilter);
    float top = min.y + S(64.0f);
    if (!g_importError.empty()) {
        icons::Draw(d, kIconAlert, ImVec2(min.x + S(12.0f), top + S(6.0f)), S(15.0f), C(p.danger), S(1.6f));
        TextFit(d, g_fonts.tiny, ImVec2(min.x + S(28.0f), top), C(p.danger), T(g_importError.c_str()), max.x - min.x - S(40.0f));
        top += S(24.0f);
    }
    // Vitrina | guardarropa
    const float gap = S(kGap);
    const float showW = std::clamp((max.x - min.x) * 0.38f, S(320.0f), S(520.0f));
    DrawShowcase(d, ImVec2(min.x, top), ImVec2(min.x + showW, max.y));
    const float guideH = S(140.0f);
    DrawWardrobe(d, ImVec2(min.x + showW + gap + S(4.0f), top + S(4.0f)), ImVec2(max.x - S(12.0f), max.y - guideH - gap));
    DrawSkinGuide(d, ImVec2(min.x + showW + gap, max.y - guideH), max);
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
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC, para revisar la skin
    const fs::path dir = DataDir();
    std::wstring lastSkinKey;
    bool lastSlim = false;
    bool lastLegacy = false;
    while (!g_quit.load()) {
        LocalData data;
        data.game = QueryGameStatus();
        const nlohmann::json profile = ReadJson(dir / L"profile.json");
        data.profileName = profile.value("name", std::string());
        data.gameDir = profile.value("gameDir", std::string());
        // La skin: solo se vuelve a mirar la imagen si el archivo cambió.
        {
            std::error_code ec;
            const fs::path skin = dir / L"skin.png";
            const auto written = fs::last_write_time(skin, ec);
            if (!ec) {
                const std::wstring key = skin.native() + L"|" + std::to_wstring(written.time_since_epoch().count());
                if (key != lastSkinKey) {
                    std::vector<std::uint8_t> rgba;
                    unsigned w = 0;
                    unsigned h = 0;
                    if (textures::DecodeFile(skin.native(), rgba, w, h)) {
                        lastSlim = skin3d::DetectSlim(rgba.data(), static_cast<int>(w), static_cast<int>(h));
                        lastLegacy = h * 2 == w;
                        lastSkinKey = key;
                    }
                }
                if (key == lastSkinKey) {
                    data.skinPath = skin.native();
                    data.skinKey = key;
                    data.skinSlim = lastSlim;
                    data.skinLegacy = lastLegacy;
                }
            }
        }
        const nlohmann::json stats = ReadJson(dir / L"stats.json");
        data.sessions = stats.value("sessions", 0LL);
        data.playSeconds = stats.value("playSeconds", 0LL);
        data.lastPlayed = stats.value("lastPlayed", 0LL);
        if (data.lastPlayed == 0 && data.sessions > 0) {
            // Versiones viejas de Luma.dll no guardaban la fecha: usar la del archivo.
            std::error_code ec;
            const auto written = fs::last_write_time(dir / L"stats.json", ec);
            if (!ec) {
                data.lastPlayed = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch())
                                      .count();
            }
        }
        {
            std::lock_guard lock(g_dataMutex);
            g_data = std::move(data);
        }
        for (int i = 0; i < 15 && !g_quit.load(); ++i) {
            Sleep(100);
        }
    }
    CoUninitialize();
}

}  // namespace

void AppInit(HWND hwnd, float scale) {
    perf::Init();  // (the music's clock: core/media.cpp)
    g_hwnd = hwnd;
    g_scale = scale;
    LoadConfig();
    g_startWithWindows = ReadStartup();
    if (g_startWithWindows) {
        WriteStartup(true);  // por si moviste el launcher a otra carpeta: actualizar la ruta
    }
    AppRebuildFonts();
    skins::Load();
    // "--page=skins" / "--page=settings": abrir directo en esa sección (para pruebas).
    if (std::wcsstr(GetCommandLineW(), L"--page=skins") != nullptr) g_page = Page::Skins;
    if (std::wcsstr(GetCommandLineW(), L"--page=settings") != nullptr) g_page = Page::Settings;
    if (std::wcsstr(GetCommandLineW(), L"--page=lyrics") != nullptr) g_page = Page::Lyrics;
    if (std::wcsstr(GetCommandLineW(), L"--page=spotify") != nullptr) g_page = Page::Spotify;
    g_statusThread = std::thread(StatusThread);
    account::Init();
    updater::Start(g_autoUpdate, GetCommandLineW());
}

// The icons' shared texture (render/icons.h), made and updated with D3D11.
ID3D11ShaderResourceView* g_iconAtlas = nullptr;

ImTextureID CreateIconTexture(const std::uint8_t* rgba, unsigned width, unsigned height) {
    if (g_device == nullptr || g_iconAtlas != nullptr) return ImTextureID{};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{rgba, width * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, &init, &texture))) return ImTextureID{};
    g_device->CreateShaderResourceView(texture, nullptr, &g_iconAtlas);
    texture->Release();  // the view keeps it alive
    return g_iconAtlas != nullptr ? reinterpret_cast<ImTextureID>(g_iconAtlas) : ImTextureID{};
}

bool UpdateIconTexture(ImTextureID, const std::uint8_t* rgba, unsigned width, unsigned) {
    if (g_device == nullptr || g_iconAtlas == nullptr) return false;
    ID3D11Resource* resource = nullptr;
    g_iconAtlas->GetResource(&resource);
    ID3D11DeviceContext* context = nullptr;
    g_device->GetImmediateContext(&context);
    const bool ok = resource != nullptr && context != nullptr;
    if (ok) context->UpdateSubresource(resource, 0, nullptr, rgba, width * 4, 0);
    if (context != nullptr) context->Release();
    if (resource != nullptr) resource->Release();
    return ok;
}

void AppSetDevice(ID3D11Device* device) {
    g_device = device;
    icons::SetBackend(CreateIconTexture, UpdateIconTexture);
    textures::SetDevice(device);
    skin3d::Init(device);
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
    // La letra pixelada: no la distribuimos; se lee de la carpeta de TU juego
    // (Luma.dll anota cuál es en profile.json).
    g_fonts.pixel = nullptr;
    {
        const nlohmann::json profile = ReadJson(DataDir() / L"profile.json");
        g_pixelFontDir = profile.value("gameDir", std::string());
        if (!g_pixelFontDir.empty()) {
            const fs::path font = fs::path(std::u8string(reinterpret_cast<const char8_t*>(g_pixelFontDir.data()), g_pixelFontDir.size())) / L"data" / L"fonts" / L"Mojangles.ttf";
            std::error_code ec;
            if (fs::exists(font, ec)) {
                ImFontConfig config;
                config.OversampleH = 1;
                config.OversampleV = 1;
                config.PixelSnapH = true;
                g_fonts.pixel = io.Fonts->AddFontFromFileTTF(font.string().c_str(), std::round(20.0f * g_scale), &config);
            }
        }
    }
    ImFont* fallback = g_fonts.regular;
    if (g_fonts.tiny == nullptr) g_fonts.tiny = fallback;
    if (g_fonts.semibold == nullptr) g_fonts.semibold = fallback;
    if (g_fonts.title == nullptr) g_fonts.title = g_fonts.semibold;
    if (g_fonts.hero == nullptr) g_fonts.hero = g_fonts.title;
    if (g_fonts.pixel == nullptr) g_fonts.pixel = g_fonts.semibold;
    io.Fonts->Build();
}

void AppFrame() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const Palette& p = P();

    LocalData data;
    {
        std::lock_guard lock(g_dataMutex);
        data = g_data;
    }
    if (data.gameDir != g_pixelFontDir) {
        g_fontsDirty = true;  // se supo dónde está el juego: cargar su letra pixelada
        g_pixelFontDir = data.gameDir;
    }
    SyncSkins(data);  // si Luma.dll guardó una skin nueva, sumarla a la biblioteca
    const account::Info acc = account::Get();
    UpdateAvatar(acc);
    g_upd = updater::Get();
    // La versión nueva ya se abrió: cerrar esta.
    if (g_upd.state == updater::State::Restarting && !g_closing) {
        g_closing = true;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    const std::string name = PlayerName(acc, data);
    // The music (only listened to while the window is on screen).
    const bool visible = IsIconic(g_hwnd) == FALSE && IsWindowVisible(g_hwnd) != FALSE;
    const media::Track track = media::Current();
    UpdateMusic(track, visible);

    // Al terminar JUGAR con éxito: minimizar (un instante después, para que
    // se alcance a ver "Luma está funcionando"), si así está en Ajustes.
    const PlayState state = g_flow.Snapshot().state;
    if (state != g_lastState) {
        if (state == PlayState::Done && g_minimizeAfterPlay) {
            g_minimizeAt = ImGui::GetTime() + 1.5;
        }
        g_lastState = state;
    }
    if (g_minimizeAt > 0.0 && ImGui::GetTime() >= g_minimizeAt) {
        g_minimizeAt = 0.0;
        PostMessageW(g_hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
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
    DrawAmbient(d, display, track);

    g_fade = 1.0f;
    // THE STAGE: no sidebar; the song in the middle, the dock and the PLAY
    // orb floating, the other sections as a sheet over it.
    g_pageFade = std::min(1.0f, g_pageFade + Dt() * 5.0f);
    DrawStage(d, display, track, data, name, acc);
    DrawPlayMenu(d);
    if (g_upd.state == updater::State::Downloading || g_upd.state == updater::State::Installing ||
        g_upd.state == updater::State::Restarting) {
        DrawUpdateOverlay(d, display);
    }
    DrawTitleBar(d, display);
    DrawStageTop(d, display, data, name, acc);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void AppShutdown() {
    g_quit.store(true);
    audioviz::Shutdown();
    media::Shutdown();
    ReleaseArt(g_art);
    ReleaseArt(g_artOld);
    ReleaseArt(g_blur);
    ReleaseArt(g_blurOld);
    if (g_lightTex != nullptr) {
        g_lightTex->Release();
        g_lightTex = nullptr;
    }
    icons::Shutdown();
    if (g_iconAtlas != nullptr) {
        g_iconAtlas->Release();
        g_iconAtlas = nullptr;
    }
    account::Shutdown();
    updater::Shutdown();
    if (g_statusThread.joinable()) {
        g_statusThread.join();
    }
    if (g_avatar != nullptr) {
        g_avatar->Release();
        g_avatar = nullptr;
    }
    textures::ReleaseAll();
    skin3d::Shutdown();
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
