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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "account.h"
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
constexpr const char* kIconHome = "M3.5 10.5L12 3.5l8.5 7 M5.5 9v11h13V9 M10 20v-6h4v6";
constexpr const char* kIconAddons = "M12 3l8 4.5v9L12 21l-8-4.5v-9z M12 12l8-4.5 M12 12v9 M12 12L4 7.5";  // un bloque
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
constexpr const char* kIconClock = "M3.5 12C3.5 7.3 7.3 3.5 12 3.5S20.5 7.3 20.5 12S16.7 20.5 12 20.5S3.5 16.7 3.5 12z M12 7.5V12l3 2";
constexpr const char* kIconSessions = "M4 6h16v12H4z M4 10h16 M8 14h3";
constexpr const char* kIconNews = "M5 4h11l3 3v13H5z M8 9h8 M8 12.5h8 M8 16h5";
constexpr const char* kIconSliders = "M4 6.5h9 M17 6.5h3 M15 4.5v4 M4 12h3 M11 12h9 M9 10v4 M4 17.5h11 M19 17.5h1 M17 15.5v4";
constexpr const char* kIconCalendar = "M4.5 6h15v14h-15z M4.5 10h15 M8.5 3.5v4 M15.5 3.5v4";
constexpr const char* kIconWindows = "M4 5.5l7-1v7H4z M13 4.2l7-1v8.3h-7z M4 13h7v7l-7-1z M13 13h7v8.3l-7-1z";
constexpr const char* kIconStar = "F M12 3.5l2.6 5.4 5.9.8-4.3 4.1 1 5.8L12 16.9l-5.2 2.7 1-5.8-4.3-4.1 5.9-.8z";
constexpr const char* kIconChevron = "M9.5 6l6 6-6 6";
constexpr const char* kIconFolder = "M3.5 6.5h6l2 2h9v10h-17z";
constexpr const char* kIconUser = "M8 8C8 5.8 9.8 4 12 4S16 5.8 16 8S14.2 12 12 12S8 10.2 8 8z M4.5 20c.8-3.8 3.9-6 7.5-6s6.7 2.2 7.5 6";

// --- Medidas (a 100% de escala; se multiplican por g_scale) ---------------------
constexpr float kSidebarWidth = 232.0f;
constexpr float kTitleBarHeight = 48.0f;
constexpr float kPad = 24.0f;      // margen del contenido
constexpr float kGap = 18.0f;      // separación entre tarjetas
constexpr float kRound = 14.0f;    // esquinas de las tarjetas

enum class Page { Home = 0, Addons, Skins, Settings };

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
void DrawSidebar(ImDrawList* d, ImVec2 display, const account::Info& acc, const LocalData& data) {
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
    icons::Draw(d, g_playMenuOpen ? "M7 14l5-5 5 5" : "M7 10l5 5 5-5", ImVec2((dmin.x + dmax.x) * 0.5f, (dmin.y + dmax.y) * 0.5f),
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
            std::snprintf(countText, sizeof(countText), T("skins.count"), static_cast<int>(skins::List().size()));
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
        {"##skin_spin", "M18.5 12c0 3.6-2.9 6.5-6.5 6.5S5.5 15.6 5.5 12 8.4 5.5 12 5.5c2 0 3.7.9 4.9 2.3 M17.5 4v4h-4", g_cardSpin, 0},
        {"##skin_zoom", "M10.5 4.5c3.3 0 6 2.7 6 6s-2.7 6-6 6-6-2.7-6-6 2.7-6 6-6z M15 15l5 5 M8 10.5h5 M10.5 8v5", g_cardZoomTarget > 1.0f, 1},
        {"##skin_full", "M4 9V4h5 M15 4h5v5 M20 15v5h-5 M9 20H4v-5", false, 2},
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

// --- Sección Skins ------------------------------------------------------------------
std::string g_selectedSkin;          // id de la skin elegida en la sección
std::string g_currentGameSkin;       // id de la skin que usas ahora en el juego
std::wstring g_lastSyncedKey;        // para agregar a la biblioteca solo cuando cambia skin.png
int g_skinFilter = 0;                // 0 = todas, 1 = favoritas
float g_skinScroll = 0.0f;
float g_skinsYaw = -0.45f;
double g_deleteArmedAt = -10.0;      // borrar pide un segundo clic (dentro de 3 s)
std::string g_importError;

// Lo que pasa cada frame aunque no estés en la sección: si Luma.dll guardó
// una skin nueva, sumarla a la biblioteca.
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

void DrawSkinsGrid(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("skins.title"));
    // Arriba a la derecha: importar y el filtro.
    float importW = 0.0f;
    const char* importLabel = T("skins.import");
    importW = TextSize(g_fonts.semibold, importLabel).x + S(32.0f);
    if (Button("##skins_import", ImVec2(max.x - S(20.0f) - importW, min.y + S(12.0f)), importLabel, true)) {
        const std::string id = skins::ImportWithDialog(g_hwnd, g_importError);
        if (!id.empty()) g_selectedSkin = id;
    }
    const char* filters[2] = {T("skins.all"), T("skins.favorites")};
    const float fw = SegmentedWidth(filters, 2);
    Segmented("##skins_filter", ImVec2(max.x - S(30.0f) - importW - fw, min.y + S(12.0f)), filters, 2, &g_skinFilter);
    float top = min.y + S(62.0f);
    if (!g_importError.empty()) {
        icons::Draw(d, kIconAlert, ImVec2(min.x + S(28.0f), top + S(8.0f)), S(15.0f), C(p.danger), S(1.6f));
        TextFit(d, g_fonts.tiny, ImVec2(min.x + S(42.0f), top), C(p.danger), T(g_importError.c_str()), max.x - min.x - S(64.0f));
        top += S(24.0f);
    }

    // La cuadrícula (con su propio recorte: lo que queda fuera no se ve ni se puede tocar).
    const ImVec2 gmin(min.x + S(16.0f), top);
    const ImVec2 gmax(max.x - S(16.0f), max.y - S(16.0f));
    const float gap = S(12.0f);
    const int columns = std::max(1, static_cast<int>((gmax.x - gmin.x + gap) / (S(150.0f) + gap)));
    const float tileW = ((gmax.x - gmin.x) - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float tileH = tileW * 1.3f;
    std::vector<const skins::Skin*> shown;
    for (const skins::Skin& s : skins::List()) {
        if (g_skinFilter == 0 || s.favorite) shown.push_back(&s);
    }
    if (shown.empty()) {
        const char* text = g_skinFilter == 1 ? T("skins.no_favorites") : T("skins.empty");
        const ImVec2 ts = TextSize(g_fonts.regular, text);
        Text(d, g_fonts.regular, ImVec2((gmin.x + gmax.x - ts.x) * 0.5f, (gmin.y + gmax.y) * 0.5f - ts.y), C(p.textDim), text);
        return;
    }
    const int rows = (static_cast<int>(shown.size()) + columns - 1) / columns;
    const float contentH = rows * tileH + (rows - 1) * gap;
    const float maxScroll = std::max(0.0f, contentH - (gmax.y - gmin.y));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x >= gmin.x && mouse.x < gmax.x && mouse.y >= gmin.y && mouse.y < gmax.y) {
        g_skinScroll -= ImGui::GetIO().MouseWheel * S(70.0f);
    }
    g_skinScroll = std::clamp(g_skinScroll, 0.0f, maxScroll);
    ImGui::PushClipRect(gmin, gmax, true);
    const float t = static_cast<float>(ImGui::GetTime());
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const skins::Skin& s = *shown[i];
        const int col = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const ImVec2 tmin(gmin.x + col * (tileW + gap), gmin.y + row * (tileH + gap) - g_skinScroll);
        const ImVec2 tmax(tmin.x + tileW, tmin.y + tileH);
        if (tmax.y < gmin.y || tmin.y > gmax.y) continue;  // fuera de la vista: ni se dibuja
        char id[48];
        std::snprintf(id, sizeof(id), "##skin_%s", s.id.substr(0, 12).c_str());
        float hover = 0.0f;
        if (Hit(id, tmin, tmax, &hover)) {
            g_selectedSkin = s.id;
            g_deleteArmedAt = -10.0;
        }
        const bool selected = s.id == (SelectedSkin() ? SelectedSkin()->id : std::string());
        d->AddRectFilled(tmin, tmax, C(Mix(p.background, p.cardHover, hover)), S(12.0f));
        d->AddRect(tmin, tmax, C(selected ? p.accent : Mix(p.border, p.accent, hover * 0.3f)), S(12.0f), 0,
                   selected ? S(2.0f) : 1.0f);
        skin3d::Model model;
        if (LibraryModel(s, model)) {
            skin3d::View view;
            view.yaw = -0.45f + (hover > 0.0f ? std::sin(t * 1.5f) * 0.6f * hover : 0.0f);
            view.time = t;
            view.alpha = g_fade;
            skin3d::Draw(d, ImVec2(tmin.x + S(10.0f), tmin.y + S(14.0f)), ImVec2(tmax.x - S(10.0f), tmax.y - S(38.0f)), model,
                         view);
        }
        TextFit(d, g_fonts.tiny, ImVec2(tmin.x + S(10.0f), tmax.y - S(26.0f)), C(p.text), s.name.c_str(), tileW - S(20.0f));
        if (s.favorite) {
            icons::Draw(d, kIconStar, ImVec2(tmax.x - S(16.0f), tmin.y + S(16.0f)), S(15.0f), C(p.warning));
        }
        if (s.id == g_currentGameSkin) {
            const char* badge = T("skins.current");
            const ImVec2 bs = TextSize(g_fonts.tiny, badge);
            const ImVec2 bmin(tmin.x + S(8.0f), tmin.y + S(8.0f));
            d->AddRectFilled(bmin, ImVec2(bmin.x + bs.x + S(14.0f), bmin.y + bs.y + S(6.0f)), C(p.success, 0.9f), S(6.0f));
            Text(d, g_fonts.tiny, ImVec2(bmin.x + S(7.0f), bmin.y + S(3.0f)), C(ImVec4(0.08f, 0.1f, 0.09f, 1)), badge);
        }
    }
    ImGui::PopClipRect();
    // Barra de desplazamiento (si hay más de lo que entra).
    if (maxScroll > 0.0f) {
        const float viewH = gmax.y - gmin.y;
        const float barH = std::max(S(30.0f), viewH * viewH / contentH);
        const float barY = gmin.y + (viewH - barH) * (g_skinScroll / maxScroll);
        d->AddRectFilled(ImVec2(gmax.x + S(5.0f), barY), ImVec2(gmax.x + S(9.0f), barY + barH), C(p.textDim, 0.4f), S(2.0f));
    }
}

void DrawSkinDetail(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    const skins::Skin* s = SelectedSkin();
    if (s == nullptr) {
        SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("skin.title"));
        TextWrap(d, g_fonts.regular, ImVec2(min.x + S(22.0f), min.y + S(60.0f)), C(p.textDim), T("skins.empty"),
                 max.x - min.x - S(44.0f));
        return;
    }
    // El personaje grande sobre el cielo, se gira arrastrando.
    const float viewH = (max.y - min.y) * 0.62f;
    const ImVec2 vmin(min.x + S(12.0f), min.y + S(12.0f));
    const ImVec2 vmax(max.x - S(12.0f), min.y + viewH);
    sky::Draw(d, vmin, vmax, S(4.0f), S(12.0f), C(p.card), g_skyMode);
    DragToRotate("##skins_view", vmin, vmax, g_skinsYaw);
    skin3d::Model model;
    if (LibraryModel(*s, model)) {
        skin3d::View view;
        view.yaw = g_skinsYaw;
        view.time = static_cast<float>(ImGui::GetTime());
        view.alpha = g_fade;
        const float h = (vmax.y - vmin.y) * 0.86f;
        const ImVec2 c((vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f);
        ImGui::PushClipRect(vmin, vmax, true);
        skin3d::Draw(d, ImVec2(c.x - h * 0.4f, c.y - h * 0.5f), ImVec2(c.x + h * 0.4f, c.y + h * 0.5f), model, view);
        ImGui::PopClipRect();
    }
    // Datos
    const float x = min.x + S(22.0f);
    const float w = max.x - min.x - S(44.0f);
    float y = vmax.y + S(16.0f);
    TextFit(d, g_fonts.title, ImVec2(x, y), C(p.text), s->name.c_str(), w);
    y += g_fonts.title->FontSize + S(4.0f);
    char info[160];
    std::snprintf(info, sizeof(info), "%s  ·  %s", s->fromGame ? T("skins.from_game") : T("skins.imported"),
                  s->legacy ? T("skins.legacy") : (s->slim ? T("skins.slim") : T("skins.classic")));
    TextFit(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), info, w);
    y += g_fonts.tiny->FontSize + S(14.0f);
    // Botones
    float bx = x;
    float bw = 0.0f;
    if (Button("##skin_fav", ImVec2(bx, y), s->favorite ? T("skins.unfavorite") : T("skins.favorite"), false, &bw)) {
        skins::SetFavorite(s->id, !s->favorite);
    }
    bx += bw + S(8.0f);
    if (Button("##skin_folder", ImVec2(bx, y), T("skins.open_folder"), false, &bw)) {
        OpenFolder(skins::Folder());
    }
    bx += bw + S(8.0f);
    const bool armed = ImGui::GetTime() - g_deleteArmedAt < 3.0;
    if (Button("##skin_delete", ImVec2(bx, y), armed ? T("skins.confirm_delete") : T("skins.delete"), false, &bw)) {
        if (armed) {
            const std::string id = s->id;
            g_deleteArmedAt = -10.0;
            g_selectedSkin.clear();
            skins::Remove(id);
            return;
        }
        g_deleteArmedAt = ImGui::GetTime();
    }
    y += S(50.0f);
    // Usar en Minecraft: llega con los cosméticos.
    Button("##skin_use", ImVec2(x, y), T("skins.use_in_game"), true, &bw, false);
    TextWrap(d, g_fonts.tiny, ImVec2(x + bw + S(12.0f), y + S(4.0f)), C(p.textDim), T("skins.use_soon"), w - bw - S(12.0f));
}

void DrawSkinsPage(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const float gap = S(kGap);
    const float leftW = (max.x - min.x) * 0.58f;
    DrawSkinsGrid(d, min, ImVec2(min.x + leftW, max.y));
    DrawSkinDetail(d, ImVec2(min.x + leftW + gap, min.y), max);
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
    g_hwnd = hwnd;
    g_scale = scale;
    LoadConfig();
    g_startWithWindows = ReadStartup();
    if (g_startWithWindows) {
        WriteStartup(true);  // por si moviste el launcher a otra carpeta: actualizar la ruta
    }
    AppRebuildFonts();
    skins::Load();
    g_statusThread = std::thread(StatusThread);
    account::Init();
    updater::Start(g_autoUpdate, GetCommandLineW());
}

void AppSetDevice(ID3D11Device* device) {
    g_device = device;
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

    g_fade = 1.0f;
    DrawSidebar(d, display, acc, data);

    // Contenido (con un fundido al cambiar de sección)
    g_pageFade = std::min(1.0f, g_pageFade + Dt() * 5.0f);
    const float appear = g_pageFade * g_pageFade * (3.0f - 2.0f * g_pageFade);
    g_fade = appear;
    const float offset = (1.0f - appear) * S(10.0f);
    const ImVec2 cmin(S(kSidebarWidth) + S(kPad), S(kTitleBarHeight) + offset);
    const ImVec2 cmax(display.x - S(kPad), display.y - S(kPad) + offset);
    switch (g_page) {
        case Page::Home: DrawHome(d, cmin, cmax, data, name); break;
        case Page::Addons: DrawComingSoon(d, cmin, cmax, kIconAddons, "nav.addons", "soon.addons"); break;
        case Page::Skins: DrawSkinsPage(d, cmin, cmax); break;
        case Page::Settings: DrawSettings(d, cmin, cmax, acc, data); break;
    }
    g_fade = 1.0f;
    if (g_page == Page::Home) {
        DrawPlayMenu(d);
        DrawSkinFullscreen(d, display, data);
    } else {
        g_playMenuOpen = false;
        g_skinFullscreen = false;
    }
    if (g_upd.state == updater::State::Downloading || g_upd.state == updater::State::Installing ||
        g_upd.state == updater::State::Restarting) {
        DrawUpdateOverlay(d, display);
    }
    DrawTitleBar(d, display);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void AppShutdown() {
    g_quit.store(true);
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
