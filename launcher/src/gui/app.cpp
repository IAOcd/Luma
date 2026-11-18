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
#include <wincodec.h>
#include <shellapi.h>
#include <shlobj.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>  // (to draw "your year" into a picture)
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
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "account.h"
#include "cloud_sync.h"
#include "gallery.h"
#include "listening.h"
#include "smtc_bridge.h"
#include "spotify_engine.h"
#include "core/audio_viz.h"
#include "core/lyrics.h"
#include "core/spotify.h"
#include "flow_presets.h"
#include "flow_styles.h"
#include "render/font_fallback.h"
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
    ImFont* lyric = nullptr;     // the lyrics view's big lines
    ImFont* line = nullptr;      // the line being sung, under the song's name
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

// The Minecraft versions Luma works with: the published release's list (from
// GitHub) PLUS the one built into this launcher (release/minecraft.json), so a
// version added there isn't "unsupported" while the release isn't out yet.
std::vector<std::string> SupportedMinecraft();
PlayState g_lastState = PlayState::Idle;
double g_minimizeAt = 0.0;  // cuándo minimizar después de JUGAR (0 = no)

// Ajustes del launcher (launcher.json)
bool g_minimizeAfterPlay = true;
bool g_startWithWindows = false;  // la verdad está en el registro: se lee al abrir
bool g_autoUpdate = true;         // actualizarse solo al abrir
bool g_liveBackground = true;     // the album's colors flow in the background (Settings > Music)
bool g_homeLyrics = true;         // the lyrics under the song's name on Home
sky::Mode g_skyMode = sky::Mode::Auto;  // el cielo del cartel: según la hora o fijo
updater::Info g_upd;              // el estado del actualizador en este frame

std::vector<std::string> SupportedMinecraft() {
    std::vector<std::string> list = g_upd.supportedMinecraft;
#ifdef LUMA_BUILTIN_MINECRAFT
    if (!list.empty()) {  // (empty = not known: no warning at all, as before)
        const std::string built = LUMA_BUILTIN_MINECRAFT;
        std::size_t pos = 0;
        while (pos <= built.size()) {
            std::size_t end = built.find(',', pos);
            if (end == std::string::npos) end = built.size();
            const std::string v = built.substr(pos, end - pos);
            if (!v.empty() && std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
            pos = end + 1;
        }
    }
#endif
    return list;
}

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
    g_liveBackground = json.value("liveBackground", true);
    g_homeLyrics = json.value("homeLyrics", true);
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
    json["liveBackground"] = g_liveBackground;
    json["homeLyrics"] = g_homeLyrics;
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

// Fades and moves what was drawn since vertex "start" (menus that open,
// cards that come in): every point of it gets "alpha" times its own, moved by "offset".
void AnimateSince(ImDrawList* d, int start, float alpha, ImVec2 offset) {
    if (alpha >= 0.999f && offset.x == 0.0f && offset.y == 0.0f) return;
    for (int i = start; i < d->VtxBuffer.Size; ++i) {
        ImDrawVert& v = d->VtxBuffer[i];
        v.pos.x += offset.x;
        v.pos.y += offset.y;
        const unsigned a = static_cast<unsigned>(static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

// THE BLURRED LETTERS. Real blur, cheap: when the fonts are built, the picture
// that holds every letter (the "atlas") is copied twice and blurred on the CPU
// (once a little, once more). Drawing a text with one of those copies instead
// of the sharp one gives blurred letters, at the cost of normal text. The
// letters are kept apart in the atlas (padding) so their blur doesn't touch.
// Used for the far lines of the lyrics and for the glow of the sung words.
constexpr int kAtlasPadding = 24;
ID3D11ShaderResourceView* g_blurAtlas[2] = {nullptr, nullptr};

void ReleaseBlurAtlas() {
    for (auto*& v : g_blurAtlas) {
        if (v != nullptr) v->Release();
        v = nullptr;
    }
}

// A blur of "radius" pixels on one alpha picture: three box blurs one way
// then the other (looks like a gaussian). "rx" and "ry" can differ (the atlas
// is twice as fine horizontally for most fonts).
void BlurAlpha(std::vector<float>& img, int w, int h, int rx, int ry) {
    std::vector<float> tmp(img.size());
    auto pass = [&](int r, bool horizontal) {
        if (r <= 0) return;
        const float inv = 1.0f / static_cast<float>(2 * r + 1);
        const int lines = horizontal ? h : w, len = horizontal ? w : h;
        for (int l = 0; l < lines; ++l) {
            auto at = [&](int i) -> float& { return horizontal ? img[static_cast<std::size_t>(l * w + i)] : img[static_cast<std::size_t>(i * w + l)]; };
            auto out = [&](int i) -> float& { return horizontal ? tmp[static_cast<std::size_t>(l * w + i)] : tmp[static_cast<std::size_t>(i * w + l)]; };
            float sum = 0.0f;
            for (int i = -r; i <= r; ++i) sum += at(std::clamp(i, 0, len - 1));
            for (int i = 0; i < len; ++i) {
                out(i) = sum * inv;
                sum += at(std::min(i + r + 1, len - 1)) - at(std::max(i - r, 0));
            }
        }
        img.swap(tmp);
    };
    for (int k = 0; k < 3; ++k) pass(rx, true);
    for (int k = 0; k < 3; ++k) pass(ry, false);
}

// The two blurred copies of the letters' picture, ready to be sent to the GPU.
// (Slow on a big atlas, about 2 s: it runs in the fonts' helper thread.)
void BlurLetters(const unsigned char* alpha, int w, int h, std::vector<std::uint8_t> out[2]) {
    // (horizontal, vertical) per level: a little, more. Three box passes spread
    // about 3x the radius: it must stay inside the padding, or a neighbor letter's
    // blur shows as the edge of a box.
    const int radii[2][2] = {{2, 1}, {3, 3}};
    for (int level = 0; level < 2; ++level) {
        std::vector<float> img(static_cast<std::size_t>(w) * h);
        for (std::size_t i = 0; i < img.size(); ++i) img[i] = alpha[i] / 255.0f;
        BlurAlpha(img, w, h, radii[level][0], radii[level][1]);
        out[level].assign(img.size() * 4, 255);
        for (std::size_t i = 0; i < img.size(); ++i) out[level][i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(img[i] * 1.4f, 0.0f, 1.0f) * 255.0f);
    }
}

// A line of text with blurred letters (level 0 = a little, 1 = more). Each
// letter's square is drawn a bit bigger than usual, so its blur isn't cut.
void BlurText(ImDrawList* d, ImFont* font, float size, ImVec2 pos, ImU32 color, const char* text, const char* textEnd, int level) {
    ID3D11ShaderResourceView* tex = g_blurAtlas[std::clamp(level, 0, 1)];
    if (tex == nullptr || ((color >> IM_COL32_A_SHIFT) & 0xFF) == 0) return;
    if (textEnd == nullptr) textEnd = text + std::strlen(text);
    const ImFontAtlas* atlas = font->ContainerAtlas;
    const float scale = size / font->FontSize;
    // (half the gap: the other half belongs to the neighbor letter)
    const float pad = static_cast<float>(kAtlasPadding / 2) - 1.0f;
    d->PushTextureID(reinterpret_cast<ImTextureID>(tex));
    float x = pos.x;
    for (const char* s = text; s < textEnd;) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, s, textEnd);
        if (n <= 0) break;
        s += n;
        const ImFontGlyph* g = font->FindGlyph(static_cast<ImWchar>(c));
        if (g == nullptr) continue;
        if (g->Visible && g->U1 > g->U0 && g->V1 > g->V0) {
            const float gw = (g->X1 - g->X0) * scale, gh = (g->Y1 - g->Y0) * scale;
            const float tw = (g->U1 - g->U0) * static_cast<float>(atlas->TexWidth), th = (g->V1 - g->V0) * static_cast<float>(atlas->TexHeight);
            const float px = gw / tw * pad, py = gh / th * pad;  // the padding, in screen pixels
            const float pu = pad / static_cast<float>(atlas->TexWidth), pv = pad / static_cast<float>(atlas->TexHeight);
            const ImVec2 a(x + g->X0 * scale - px, pos.y + g->Y0 * scale - py), b(x + g->X1 * scale + px, pos.y + g->Y1 * scale + py);
            d->PrimReserve(6, 4);
            d->PrimRectUV(a, b, ImVec2(g->U0 - pu, g->V0 - pv), ImVec2(g->U1 + pu, g->V1 + pv), color);
        }
        x += g->AdvanceX * scale;
    }
    d->PopTextureID();
}

// How tall a wrapped text is (without drawing it).
float TextWrapHeight(ImFont* font, const char* text, float width) {
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, width, text).y;
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
// While a section covers the stage, what's behind it can't be touched.
bool g_blockInput = false;

bool Hit(const char* id, ImVec2 min, ImVec2 max, float* hover, bool enabled = true) {
    if (g_blockInput) {
        ImGui::SetCursorScreenPos(min);
        ImGui::Dummy(ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));  // (an item that does nothing)
        if (hover != nullptr) *hover = Anim(ImGui::GetID(id), false);
        return false;
    }
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
    float widths[8]{};
    count = std::min(count, 8);
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
    // the chosen one: a pill that slides to it (and stretches to its width)
    {
        float tx = pos.x + S(3.0f), tw = widths[0];
        for (int i = 0; i < count; ++i) {
            if (i == *index) {
                tw = widths[i];
                break;
            }
            tx += widths[i];
        }
        const ImGuiID sid = ImGui::GetID(id);
        float& px = *ImGui::GetStateStorage()->GetFloatRef(sid + 31, -1.0f);
        float& pw = *ImGui::GetStateStorage()->GetFloatRef(sid + 32, -1.0f);
        if (px < 0.0f || std::fabs(px - tx) > S(600.0f)) {
            px = tx;
            pw = tw;
        }
        const float k = std::min(1.0f, Dt() * 14.0f);
        px += (tx - px) * k;
        pw += (tw - pw) * k;
        d->AddRectFilled(ImVec2(px, pos.y + S(3.0f)), ImVec2(px + pw, pos.y + h - S(3.0f)), C(p.accent), S(8.0f));
    }
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

void DrawPlayMenuBody(ImDrawList* d);

// (it opens and closes with a short fade and slide)
void DrawPlayMenu(ImDrawList* d) {
    static float anim = 0.0f;
    anim += ((g_playMenuOpen ? 1.0f : 0.0f) - anim) * std::min(1.0f, Dt() * 16.0f);
    if (anim < 0.01f) return;
    const bool wasOpen = g_playMenuOpen;
    g_playMenuOpen = true;  // (drawn while it closes too)
    const int start = d->VtxBuffer.Size;
    DrawPlayMenuBody(d);
    if (!wasOpen) g_playMenuOpen = false;
    const float e = 1.0f - (1.0f - anim) * (1.0f - anim) * (1.0f - anim);
    AnimateSince(d, start, e, ImVec2(0.0f, (1.0f - e) * S(10.0f)));
}

void DrawPlayMenuBody(ImDrawList* d) {
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
        g_flow.Start(SupportedMinecraft());
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
                g_flow.Start(SupportedMinecraft(), true);
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

// The album art on the GPU, with its smaller copies (mipmaps): the 1024 px
// picture is drawn at ~400-600 px, and shrunk with no mipmaps its fine detail
// shimmers and looks jagged. The GPU makes them (GenerateMips).
ID3D11ShaderResourceView* MakeArt(const std::vector<std::uint8_t>& rgba) {
    const int n = media::ArtSize();
    if (g_device == nullptr || rgba.size() < static_cast<std::size_t>(n) * n * 4) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(n);
    desc.Height = static_cast<UINT>(n);
    desc.MipLevels = 0;  // (all of them, down to 1x1)
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &texture))) return nullptr;
    if (SUCCEEDED(g_device->CreateShaderResourceView(texture, nullptr, &view))) {
        ID3D11DeviceContext* context = nullptr;
        g_device->GetImmediateContext(&context);
        context->UpdateSubresource(texture, 0, nullptr, rgba.data(), static_cast<UINT>(n * 4), 0);
        context->GenerateMips(view);
        context->Release();
    }
    texture->Release();
    return view;
}

// ImGui's sampler only reads the full-size picture (it ignores mipmaps), so
// the album art is drawn with one of ours that uses them (a draw callback
// switches to it; ImGui's own comes back right after).
ID3D11SamplerState* g_mipSampler = nullptr;

void UseMipSampler(const ImDrawList*, const ImDrawCmd*) {
    if (g_device == nullptr) return;
    if (g_mipSampler == nullptr) {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        g_device->CreateSamplerState(&desc, &g_mipSampler);
    }
    ID3D11DeviceContext* context = nullptr;
    g_device->GetImmediateContext(&context);
    context->PSSetSamplers(0, 1, &g_mipSampler);
    context->Release();
}

void DrawArtImage(ImDrawList* d, ID3D11ShaderResourceView* view, ImVec2 a, ImVec2 b, ImU32 color, float rounding) {
    d->AddCallback(UseMipSampler, nullptr);
    d->AddImageRounded(reinterpret_cast<ImTextureID>(view), a, b, ImVec2(0, 0), ImVec2(1, 1), color, rounding);
    d->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
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
ArtTexture g_blobs;     // the art a little less blurred: the shapes that turn (Liner style)
ArtTexture g_blobsOld;
float g_ambient = 0.0f;  // 0..1: the ambient on (there's music)

// (shrinkTo = how many pixels it's shrunk to, passes = how many softening passes:
// 24 and 3 for the calm base; fewer for the "blobs" that turn, with more shape)
ID3D11ShaderResourceView* MakeBlur(const std::vector<std::uint8_t>& rgba, int shrinkTo = 24, int passes = 3) {
    const int kSmall = shrinkTo;
    const int n = media::ArtSize();
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
    for (int pass = 0; pass < passes; ++pass) {
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
    // 1. The blurred art, covering the window, and ALIVE (like Liner or Apple
    // Music): over it, three more huge copies of the blurred art turn slowly,
    // each around its own center, at its own speed and the other way, so the
    // album's colors flow into each other all the time (also when paused; a
    // little brighter with the bass). Only the background moves: the art, the
    // words and the buttons stay still. The copies are so big their edges
    // never enter the window.
    {
        const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
        auto layer = [&](ID3D11ShaderResourceView* view, float alpha, float size, float angle, ImVec2 center) {
            if (view == nullptr || alpha <= 0.0f) return;
            const float h = size * 0.5f, ca = std::cos(angle) * h, sa = std::sin(angle) * h;
            const ImVec2 p1(center.x - ca + sa, center.y - sa - ca), p2(center.x + ca + sa, center.y + sa - ca);
            const ImVec2 p3(center.x + ca - sa, center.y + sa + ca), p4(center.x - ca - sa, center.y - sa + ca);
            d->AddImageQuad(reinterpret_cast<ImTextureID>(view), p1, p2, p3, p4, ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                            Col(ImVec4(1, 1, 1, 1), alpha));
        };
        if (g_blur.view == nullptr) {
            d->AddRectFilled(ImVec2(0, 0), display, Col(p.accent, 0.35f * g_ambient));
        }
        struct Spin {
            float size, speed, orbit, orbitSpeed, phase, alpha;
        };
        static const Spin kSpins[3] = {
            {2.0f, 0.045f, 0.10f, 0.060f, 0.0f, 0.65f},
            {1.8f, -0.060f, 0.14f, -0.045f, 2.1f, 0.50f},
            {1.7f, 0.075f, 0.12f, 0.050f, 4.2f, 0.40f},
        };
        const float glow = 1.0f + 0.10f * g_bassSmooth;
        auto all = [&](ID3D11ShaderResourceView* view, ID3D11ShaderResourceView* blobs, float alpha) {
            if (view == nullptr || alpha <= 0.0f) return;
            layer(view, alpha, big * 2.2f, 0.0f, c);  // (the still base)
            if (!g_liveBackground) return;  // (Settings > Music: a still background)
            if (blobs != nullptr) view = blobs;
            for (const Spin& sp : kSpins) {
                const float orbitA = now * sp.orbitSpeed + sp.phase;
                const ImVec2 oc(c.x + std::cos(orbitA) * big * sp.orbit, c.y + std::sin(orbitA) * big * sp.orbit * 0.7f);
                layer(view, std::min(1.0f, alpha * sp.alpha * glow), big * sp.size, now * sp.speed + sp.phase, oc);
            }
        };
        all(g_blurOld.view, g_blobsOld.view, g_ambient * (1.0f - g_artFade));
        all(g_blur.view, g_blobs.view, g_ambient * (g_blurOld.view != nullptr ? g_artFade : 1.0f));
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
    const float light = ThemeLight();  // (0 dark .. 1 light: it slides when the theme changes)
    const ImVec4 shade = Mix(ImVec4(0.05f, 0.05f, 0.06f, 1.0f), ImVec4(0.97f, 0.96f, 0.94f, 1.0f), light);
    const float top = (0.42f + 0.08f * light) * g_ambient, bottom = (0.72f - 0.02f * light) * g_ambient;
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
            ReleaseArt(g_blobsOld);
            g_blobsOld = g_blobs;
            g_blobs.view = MakeBlur(g_artPixels, 10, 1);
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
            ReleaseArt(g_blobsOld);
            g_blobsOld = g_blobs;
            g_blobs = ArtTexture{};
            g_artFade = 0.0f;
            g_artVersion = 0;
        }
    }
    g_artFade = std::min(1.0f, g_artFade + Dt() * 2.2f);
    if (g_artFade >= 1.0f) {
        ReleaseArt(g_artOld);
        ReleaseArt(g_blurOld);
        ReleaseArt(g_blobsOld);
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
        DrawArtImage(d, view, a, b, C(ImVec4(1, 1, 1, 1), alpha), S(18.0f));
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
        g_flow.Start(SupportedMinecraft());
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
                g_flow.Start(SupportedMinecraft(), true);
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

// =============================================================================
//  THE LYRICS (part L2)
// =============================================================================
//  1. Luma's settings, shared with the game. The launcher doesn't keep its own
//     copy of Lyric Flow's style: it reads and writes the SAME file Luma.dll
//     uses (%APPDATA%\Luma\config\profiles\<active profile>.json), and the DLL
//     notices the file changed and loads it again (core/config.cpp). So what
//     you choose here is what you see in the game, even while you're playing.
//  2. The lyrics on the stage: the line being sung, big, filling word by word
//     (karaoke), with the one before and the one after, scrolling softly.
//  3. The Lyrics sheet: the six ready-made styles (shared/flow_styles.h, the
//     same table the game uses), each one with a small animated preview.
// =============================================================================
namespace flowcfg {

namespace fstyles = luma::flowstyles;

struct State {
    bool found = false;      // the settings file could be read (or doesn't exist yet: defaults)
    bool broken = false;     // it exists but is damaged: never written over
    bool on = false;         // "lyricFlow.on"
    int mode = 0;            // "lyricFlow.mode": 0 in the world, 1 in the sky
    int style = -1;          // which ready-made style it is (-1 = your own)
    float offset = 0.0f;     // "lyricsOffset" (seconds)
    nlohmann::json look = nlohmann::json::object();  // how it looks now (to save it as your style)
    int user = -1;           // which of YOUR styles it is (-1 = none)
    std::string spotifyClientId;  // "spotifyClientId": your Spotify app (the same as the game's)
    bool moduleOn = true;    // the "Now playing" module is on (it draws them)
};
State g_state;
double g_checkedAt = -100.0;
fs::file_time_type g_when{};
fs::path g_file;

// The active profile's file (as core/config.cpp names it).
fs::path ProfileFile() {
    const fs::path dir = DataDir();
    if (dir.empty()) return {};
    const nlohmann::json root = ReadJson(dir / L"config" / L"luma.json");
    std::string name = "default";
    const auto it = root.find("activeProfile");
    if (it != root.end() && it->is_string()) name = it->get<std::string>();
    if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name.find("..") != std::string::npos) {
        name = "default";
    }
    name += ".json";
    return dir / L"config" / L"profiles" / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

// The file, parsed. "exists" = there is a file; false return = damaged.
bool ReadProfile(const fs::path& path, nlohmann::json& out, bool& exists) {
    std::error_code ec;
    exists = fs::exists(path, ec);
    out = nlohmann::json::object();
    if (!exists) return true;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out = nlohmann::json::parse(file, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        out = nlohmann::json::object();
        return false;
    }
    return true;
}

// modules > now_playing (created if it isn't there).
nlohmann::json& NowPlaying(nlohmann::json& profile) {
    nlohmann::json& modules = profile["modules"];
    if (!modules.is_object()) modules = nlohmann::json::object();
    nlohmann::json& np = modules["now_playing"];
    if (!np.is_object()) np = nlohmann::json::object();
    return np;
}
nlohmann::json& FlowOf(nlohmann::json& profile) {
    nlohmann::json& settings = NowPlaying(profile)["settings"];
    if (!settings.is_object()) settings = nlohmann::json::object();
    nlohmann::json& flow = settings["lyricFlow"];
    if (!flow.is_object()) flow = nlohmann::json::object();
    return flow;
}

// Reading a value without exceptions (a missing or wrong one = "fallback").
template <class T>
T Get(const nlohmann::json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if constexpr (std::is_same_v<T, bool>) {
        return it->is_boolean() ? it->get<bool>() : fallback;
    } else {
        return it->is_number() ? it->get<T>() : fallback;
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
bool Near3(const nlohmann::json& j, const char* key, const float* c) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number() || !Near((*it)[i].get<float>(), c[i])) return false;
    }
    return true;
}

// Which ready-made style the settings are (the same test as the game's
// CurrentFlowStyle, in render/music.cpp).
int MatchStyle(const nlohmann::json& f) {
    if (Get(f, "highlight", false) || !Near(Get(f, "opacity", fstyles::kDefaultOpacity), fstyles::kDefaultOpacity) ||
        !Near(Get(f, "outlineAlpha", fstyles::kDefaultOutlineAlpha), fstyles::kDefaultOutlineAlpha)) {
        return -1;
    }
    if (f.contains("outlineColor") && !Near3(f, "outlineColor", fstyles::kDefaultOutlineColor)) return -1;
    for (int i = 0; i < fstyles::kCount; ++i) {
        const fstyles::FlowLook& k = fstyles::kStyles[i];
        const bool colors = k.colorMode != 0 && k.colorMode != 3 ? true
                            : (Near3(f, "color", k.color) && (k.colorMode != 3 || Near3(f, "color2", k.color2)));
        const bool shadow = Get(f, "shadow", !k.shadow) == k.shadow;
        if (Get(f, "colorMode", -1) == k.colorMode && colors && Get(f, "outline", !k.outline) == k.outline && shadow &&
            (!k.shadow || Get(f, "shadowStyle", -1) == k.shadowStyle) && Get(f, "appear", -1) == k.appear &&
            Get(f, "leave", -1) == k.leave && Get(f, "motion", -1) == k.motion && Near(Get(f, "speed", -1.0f), k.speed) &&
            Get(f, "fill", -1) == k.fill && Get(f, "unsung", -1) == k.unsung && Get(f, "glow", -1) == k.glow &&
            (k.glow == 0 || (Get(f, "glowColor", -1) == k.glowColor && Near(Get(f, "glowStrength", -1.0f), k.glowStrength) &&
                             Get(f, "glowWhere", -1) == k.glowWhere)) &&
            Get(f, "vfx", -1) == k.vfx && (k.vfx == 0 || (Near(Get(f, "vfxAmount", -1.0f), k.vfxAmount) && Get(f, "vfxColor", -1) == k.vfxColor))) {
            return i;
        }
    }
    return -1;
}

// Reads the file again if it changed (about once a second: only its date is
// looked at in between).
// YOUR STYLES (shared/flow_presets.h): the file both the game and the
// launcher read, loaded again when its date changes.
std::vector<luma::flowpresets::Preset> g_presets;
fs::file_time_type g_presetsWhen{};
bool g_presetsRead = false;
int g_picked = -1;  // the style of yours you're working on (the last one applied or saved)

bool RefreshPresets(bool force) {
    const fs::path file = luma::flowpresets::FileIn(DataDir());
    std::error_code ec;
    const fs::file_time_type when = fs::last_write_time(file, ec);
    const fs::file_time_type stamp = ec ? fs::file_time_type{} : when;
    if (!force && g_presetsRead && stamp == g_presetsWhen) return false;
    g_presetsRead = true;
    g_presetsWhen = stamp;
    g_presets = luma::flowpresets::Load(file);
    return true;
}

void Refresh(bool force = false) {
    const double now = ImGui::GetTime();
    if (!force && now - g_checkedAt < 1.0) return;
    g_checkedAt = now;
    const bool presetsChanged = RefreshPresets(force);
    const fs::path path = ProfileFile();
    std::error_code ec;
    const fs::file_time_type when = fs::last_write_time(path, ec);
    if (!force && path == g_file && !ec && when == g_when && g_state.found) {
        if (presetsChanged) g_state.user = luma::flowpresets::Find(g_presets, g_state.look);
        return;
    }
    g_file = path;
    g_when = ec ? fs::file_time_type{} : when;
    nlohmann::json profile;
    bool exists = false;
    State s;
    if (!ReadProfile(path, profile, exists)) {
        s.broken = true;
        g_state = s;
        return;
    }
    s.found = true;
    const auto modules = profile.find("modules");
    if (modules != profile.end() && modules->is_object()) {
        const auto np = modules->find("now_playing");
        if (np != modules->end() && np->is_object()) {
            s.moduleOn = Get(*np, "enabled", true);
            const auto settings = np->find("settings");
            if (settings != np->end() && settings->is_object()) {
                s.offset = Get(*settings, "lyricsOffset", 0.0f);
                const auto sid = settings->find("spotifyClientId");
                if (sid != settings->end() && sid->is_string()) s.spotifyClientId = sid->get<std::string>();
                const auto flow = settings->find("lyricFlow");
                if (flow != settings->end() && flow->is_object()) {
                    s.on = Get(*flow, "on", false);
                    s.mode = std::clamp(Get(*flow, "mode", 0), 0, 1);
                    s.style = MatchStyle(*flow);
                    s.look = luma::flowpresets::LookOf(*flow);
                }
            }
        }
    }
    s.user = luma::flowpresets::Find(g_presets, s.look);
    g_state = s;
}

// Changes the file: reads it fresh, lets "change" touch Lyric Flow's part,
// and writes it safely (to a .tmp, then replacing the real one: a game that
// reads it at that moment sees the old file or the new one, never half).
nlohmann::json& SettingsOf(nlohmann::json& profile) {
    nlohmann::json& settings = NowPlaying(profile)["settings"];
    if (!settings.is_object()) settings = nlohmann::json::object();
    return settings;
}

template <class F>
bool ChangeProfile(F&& change) {
    const fs::path path = ProfileFile();
    if (path.empty()) return false;
    nlohmann::json profile;
    bool exists = false;
    if (!ReadProfile(path, profile, exists)) return false;  // damaged: not touched
    change(profile);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file << profile.dump(2);
        if (!file) return false;
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        fs::remove(temp, ec);
        return false;
    }
    Refresh(true);
    return true;
}

// Changes Lyric Flow's part.
template <class F>
bool Change(F&& change) {
    return ChangeProfile([&](nlohmann::json& profile) { change(FlowOf(profile)); });
}

// The lyrics' timing (seconds; the same setting as the game's "Offset").
void SetOffset(float seconds) {
    ChangeProfile([&](nlohmann::json& profile) { SettingsOf(profile)["lyricsOffset"] = std::round(seconds * 10.0f) / 10.0f; });
}

// Before a style replaces what you have: if it isn't saved anywhere, it's
// kept as "Your previous style" (so your config is never lost).
void BackupBeforeApply() {
    Refresh(true);
    if (!g_state.found) return;
    if (luma::flowpresets::BackupIfUnsaved(g_presets, g_state.look, g_state.style >= 0, T("lyrics.backup"))) {
        luma::flowpresets::Save(luma::flowpresets::FileIn(DataDir()), g_presets);
    }
}

// Saves what you have now as one of your styles ("My style 1"...).
void SaveCurrentStyle() {
    Refresh(true);
    if (!g_state.found || g_state.look.empty()) return;
    g_presets.push_back({luma::flowpresets::NextName(g_presets, T("lyrics.my_style")), g_state.look, false});
    g_picked = static_cast<int>(g_presets.size()) - 1;
    luma::flowpresets::Save(luma::flowpresets::FileIn(DataDir()), g_presets);
    Refresh(true);
}

// Your style "i" becomes what you have now.
void UpdateUserStyle(int i) {
    Refresh(true);
    if (i < 0 || i >= static_cast<int>(g_presets.size()) || !g_state.found || g_state.look.empty()) return;
    g_presets[static_cast<std::size_t>(i)].look = g_state.look;
    g_presets[static_cast<std::size_t>(i)].autoSaved = false;  // (yours now: the backup won't replace it)
    luma::flowpresets::Save(luma::flowpresets::FileIn(DataDir()), g_presets);
    Refresh(true);
}

void RenameUserStyle(int i, const char* name) {
    std::string n = name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(n.begin());
    if (i < 0 || i >= static_cast<int>(g_presets.size()) || n.empty()) return;
    g_presets[static_cast<std::size_t>(i)].name = n;
    g_presets[static_cast<std::size_t>(i)].autoSaved = false;  // (named by you: kept)
    luma::flowpresets::Save(luma::flowpresets::FileIn(DataDir()), g_presets);
    Refresh(true);
}

void ApplyUserStyle(int i) {
    if (i < 0 || i >= static_cast<int>(g_presets.size())) return;
    BackupBeforeApply();
    if (i >= static_cast<int>(g_presets.size())) return;
    const nlohmann::json look = g_presets[static_cast<std::size_t>(i)].look;
    Change([&](nlohmann::json& f) { luma::flowpresets::ApplyTo(f, look); });
}

void DeleteUserStyle(int i) {
    if (i < 0 || i >= static_cast<int>(g_presets.size())) return;
    g_presets.erase(g_presets.begin() + i);
    if (g_picked == i) g_picked = -1;
    else if (g_picked > i) --g_picked;
    luma::flowpresets::Save(luma::flowpresets::FileIn(DataDir()), g_presets);
    Refresh(true);
}

// A style from the gallery: added to your styles (unless you already have
// it) and put on. Its look has only "how it looks", so your lyrics stay
// where they are.
void InstallGalleryStyle(const std::string& name, const nlohmann::json& look) {
    if (!look.is_object() || look.empty()) return;
    RefreshPresets(true);
    int index = -1;
    for (std::size_t i = 0; i < g_presets.size(); ++i) {
        if (!g_presets[i].autoSaved && g_presets[i].look == look) index = static_cast<int>(i);
    }
    if (index < 0) {
        std::string unique = name.empty() ? std::string(T("lyrics.my_style")) : name;
        auto taken = [&](const std::string& n) {
            for (const auto& p : g_presets) {
                if (p.name == n) return true;
            }
            return false;
        };
        if (taken(unique)) {
            for (int n = 2;; ++n) {
                const std::string candidate = unique + " " + std::to_string(n);
                if (!taken(candidate)) {
                    unique = candidate;
                    break;
                }
            }
        }
        g_presets.push_back({unique, look, false});
        index = static_cast<int>(g_presets.size()) - 1;
        luma::flowpresets::Save(luma::flowpresets::FileIn(DataDir()), g_presets);
    }
    g_picked = index;
    ApplyUserStyle(index);
    Refresh(true);
}

void ApplyStyle(int i) {
    BackupBeforeApply();
    const fstyles::FlowLook& k = fstyles::kStyles[i];
    Change([&](nlohmann::json& f) {
        f["colorMode"] = k.colorMode;
        f["color"] = {k.color[0], k.color[1], k.color[2]};
        f["color2"] = {k.color2[0], k.color2[1], k.color2[2]};
        f["outline"] = k.outline;
        f["shadow"] = k.shadow;
        f["shadowStyle"] = k.shadowStyle;
        f["appear"] = k.appear;
        f["leave"] = k.leave;
        f["motion"] = k.motion;
        f["speed"] = k.speed;
        f["fill"] = k.fill;
        f["unsung"] = k.unsung;
        f["glow"] = k.glow;
        f["glowColor"] = k.glowColor;
        f["glowStrength"] = k.glowStrength;
        f["glowWhere"] = k.glowWhere;
        f["vfx"] = k.vfx;
        f["vfxAmount"] = k.vfxAmount;
        f["vfxColor"] = k.vfxColor;
        // (and the defaults of what the styles don't choose)
        f["highlight"] = false;
        f["opacity"] = fstyles::kDefaultOpacity;
        f["outlineColor"] = {fstyles::kDefaultOutlineColor[0], fstyles::kDefaultOutlineColor[1], fstyles::kDefaultOutlineColor[2]};
        f["outlineAlpha"] = fstyles::kDefaultOutlineAlpha;
    });
}

}  // namespace flowcfg

// --- 2. The lyrics on the stage --------------------------------------------------------
//  Two places: a short one under the song's name (the line being sung and
//  the next one), and the LYRICS VIEW (a button turns the stage into the
//  lyrics: big lines in the middle, the song small in a corner).
//  The karaoke fill has no hard edge: the sung part is bright, and the edge
//  fades over a few letters, so it glides word by word.

bool SpotifyDrivesHome(const media::Track& t, spotify::Player& out);  // (with the Spotify section, below)
void SpotifyVolumeBar(ImDrawList* d, ImVec2 left, float width, const spotify::Player& pl, float alpha);  // (below)

// Is this app Spotify? (Luma's own player, Spotify Soloist, shows in Windows
// as Luma itself: smtc_bridge.h)
bool IsSpotifyApp(const char* app) {
    if (std::strstr(app, "Spotify") != nullptr) return true;
    namespace eng = luma::launcher::spotifyengine;
    return eng::GetState() == eng::State::Ready && std::strstr(app, "Luma") != nullptr;
}

// SPOTIFY'S OWN COVER, 640 px: Windows only gets a 300 px one from Spotify (the
// big cover looked blurry). When the song plays from Spotify and the section
// knows it, its picture (already downloaded for the Spotify section) goes to
// core/media, which decodes it like the app's own. It's that album's exact
// picture (Apple Music's is looked up by name: for the other apps).
void UpdateHdArt(const media::Track& t) {
    static std::uint32_t offered = 0;
    if (!t.present || !t.artChecked || t.trackId == offered) return;
    spotify::Player sp;
    if (!SpotifyDrivesHome(t, sp) || sp.artPath.empty()) return;
    std::ifstream in(fs::path(sp.artPath), std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    offered = t.trackId;
    if (bytes.size() > 1000) media::OfferArt(t.trackId, std::move(bytes));
}

lyrics::Result g_lyrics;
std::uint32_t g_lyricsVersion = 0;
int g_lyricLine = -1;        // the line being sung
float g_lyricScroll = 1.0f;  // 0..1: the move to the new line
float g_lyricsShow = 0.0f;   // 0..1: there are lyrics to show (the short one opens)
bool g_lyricsView = false;   // the lyrics view is open
bool g_openViewWhenReady = false;
float g_lyricsViewAnim = 0.0f;

bool HasLyricLines(const media::Track& t) {
    return t.present && g_lyrics.trackId == t.trackId && g_lyrics.status == lyrics::Status::Found && g_lyrics.synced &&
           !g_lyrics.lines.empty();
}

int LyricsMs(const media::Track& t) {
    return static_cast<int>((t.position - static_cast<double>(flowcfg::g_state.offset)) * 1000.0);
}

void UpdateLyrics(const media::Track& t, bool visible) {
    lyrics::Want(lyrics::kMenu, visible && t.present);
    std::uint32_t version = g_lyricsVersion;
    if (lyrics::Snapshot(g_lyricsVersion, g_lyrics, version)) {
        g_lyricsVersion = version;
        g_lyricLine = -1;
        g_lyricScroll = 1.0f;
        // (letters of other alphabets: added to the fonts)
        for (const lyrics::Line& line : g_lyrics.lines) render::fontfallback::Need(line.text.c_str());
    }
    static std::uint32_t needTrack = 0;
    if (t.trackId != 0 && needTrack != t.trackId) {
        needTrack = t.trackId;
        render::fontfallback::Need(t.title);
        render::fontfallback::Need(t.artist);
        render::fontfallback::Need(t.album);
    }
    const bool mine = g_lyrics.trackId == t.trackId && t.present;
    const bool has = mine && (HasLyricLines(t) || g_lyrics.status == lyrics::Status::Searching ||
                              g_lyrics.status == lyrics::Status::Instrumental);
    g_lyricsShow += ((has ? 1.0f : 0.0f) - g_lyricsShow) * std::min(1.0f, Dt() * 5.0f);
    if (HasLyricLines(t)) {
        const int line = lyrics::LineAt(g_lyrics, LyricsMs(t));
        if (line != g_lyricLine) {
            // (a jump of more than one line, by seeking: no long move)
            g_lyricScroll = std::abs(line - g_lyricLine) == 1 ? 0.0f : 1.0f;
            g_lyricLine = line;
        }
    }
    g_lyricScroll = std::min(1.0f, g_lyricScroll + Dt() * 1.9f);
    // (the music stopped for a while, not just between two songs: back to the song)
    static double absentSince = -1.0;
    if (t.present) {
        absentSince = -1.0;
    } else if (absentSince < 0.0) {
        absentSince = ImGui::GetTime();
    } else if (ImGui::GetTime() - absentSince > 2.0) {
        g_lyricsView = false;
    }
    if (g_openViewWhenReady && HasLyricLines(t)) {  // ("--view=lyrics", for testing)
        g_lyricsView = true;
        g_openViewWhenReady = false;
    }
    if (g_lyricsView && ImGui::IsKeyPressed(ImGuiKey_Escape)) g_lyricsView = false;
    g_lyricsViewAnim += ((g_lyricsView ? 1.0f : 0.0f) - g_lyricsViewAnim) * std::min(1.0f, Dt() * 7.0f);
}

// How many pixels of "line" are sung at "ms" (drawn at "size"): word by word,
// each word filling while it's sung. Lines without word times: all of it.
float LineSweep(const lyrics::Line& line, int lineEnd, int ms, ImFont* font, float size, float fullWidth) {
    if (line.words.empty()) return fullWidth;
    float sweep = 0.0f, x = 0.0f;
    for (std::size_t w = 0; w < line.words.size(); ++w) {
        const lyrics::Word& word = line.words[w];
        const float ww = font->CalcTextSizeA(size, FLT_MAX, 0.0f, word.text.c_str()).x;
        int end = w + 1 < line.words.size() ? line.words[w + 1].ms : std::min(lineEnd, word.ms + 1500);
        end = std::max(end, word.ms + 1);
        if (ms >= word.ms) {
            const float f = std::clamp(static_cast<float>(ms - word.ms) / static_cast<float>(end - word.ms), 0.0f, 1.0f);
            sweep = x + ww * f;
        }
        x += ww;
    }
    return sweep;
}

// A line with the karaoke fill: dim, and bright up to "sweep" with a soft
// edge "feather" wide (each vertex of the bright letters fades by where it is).
void FillText(ImDrawList* d, ImFont* font, float size, ImVec2 pos, float height, const char* text, const ImVec4& col, float alpha,
              float dimAlpha, float sweep, float feather) {
    d->AddText(font, size, pos, C(col, dimAlpha * alpha), text);
    if (sweep <= 0.0f) return;
    const float edge = pos.x + sweep;  // where it fades out completely
    const float from = edge - feather;   // where it's still fully bright
    const int start = d->VtxBuffer.Size;
    d->PushClipRect(ImVec2(pos.x - size, pos.y - size), ImVec2(edge, pos.y + height + size), true);
    d->AddText(font, size, pos, C(col, alpha), text);
    d->PopClipRect();
    for (int i = start; i < d->VtxBuffer.Size; ++i) {
        ImDrawVert& v = d->VtxBuffer[i];
        const float k = std::clamp((edge - v.pos.x) / std::max(1.0f, feather), 0.0f, 1.0f);
        if (k >= 1.0f || v.pos.x < from) continue;
        const unsigned a = static_cast<unsigned>(static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) * k);
        v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

float EaseOutBack(float x) {  // (a soft overshoot: it arrives, goes a hair past, settles)
    x = std::clamp(x, 0.0f, 1.0f);
    const float c1 = 1.15f, c3 = c1 + 1.0f;
    return 1.0f + c3 * (x - 1.0f) * (x - 1.0f) * (x - 1.0f) + c1 * (x - 1.0f) * (x - 1.0f);
}

// THE LINE BEING SUNG, with soul. Word by word:
//   - each word fills with a soft edge while it's sung;
//   - it rises a little when it starts (and stays up: the sung part floats);
//   - the long ones (held notes, a second or more) grow a touch and get a
//     light in the album's color in the shape of their own letters, breathing
//     with the bass, and it fades when the note ends.
// Lines without word times: all bright.
void DrawSungLine(ImDrawList* d, ImFont* font, float size, ImVec2 pos, float height, const lyrics::Line& line, int lineEnd, int ms,
                  float alpha, float lift) {
    const Palette& p = P();
    if (line.words.empty()) {
        d->AddText(font, size, pos, C(p.text, alpha), line.text.c_str());
        return;
    }
    const float feather = size * 0.9f;
    float x = pos.x;
    for (std::size_t w = 0; w < line.words.size(); ++w) {
        const lyrics::Word& word = line.words[w];
        const char* text = word.text.c_str();
        const float ww = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
        int end = w + 1 < line.words.size() ? line.words[w + 1].ms : std::min(lineEnd, word.ms + 1500);
        end = std::max(end, word.ms + 1);
        const float dur = static_cast<float>(end - word.ms);
        const float f = std::clamp(static_cast<float>(ms - word.ms) / dur, 0.0f, 1.0f);
        const float up = EaseOutCubic(std::clamp(static_cast<float>(ms - word.ms) / 320.0f, 0.0f, 1.0f));
        const float dy = -lift * up;
        float held = 0.0f;  // the held note's light (0..1)
        if (dur >= 1000.0f && ms >= word.ms) {
            held = ms < end ? std::clamp(f * 4.0f, 0.0f, 1.0f) : std::clamp(1.0f - static_cast<float>(ms - end) / 450.0f, 0.0f, 1.0f);
        }
        // THE GLOW: the sung word's own letters, blurred, in a light tint of the
        // album's color, under the sharp ones (real blur: BlurText). It comes
        // in as the word starts, rests soft, and on held notes it's stronger
        // and breathes with the bass.
        if (f > 0.0f) {
            const float glow = (0.30f + 0.55f * held + 0.20f * g_bassSmooth * held) * std::clamp(f * 4.0f, 0.0f, 1.0f);
            const ImVec4 tint = Mix(p.accent, ImVec4(1, 1, 1, 1), 0.35f);
            d->PushClipRect(ImVec2(x - size, pos.y - size), ImVec2(x + (ww + feather) * f + size * 0.5f, pos.y + height + size), true);
            BlurText(d, font, size, ImVec2(x, pos.y + dy), C(tint, glow * alpha), text, nullptr, 1);
            d->PopClipRect();
        }
        if (held > 0.01f) {
            // A HELD NOTE: the letters stay sharp (never scaled: that blurs
            // them). Each letter rises in turn, a wave that goes through the
            // word as the note goes on, and a shine (the album's color, light)
            // rides on the wave's top.
            const ImVec4 shine = Mix(p.accent, ImVec4(1, 1, 1, 1), 0.55f);
            const char* end8 = text + word.text.size();
            int letters = 0;
            for (const char* c = text; c < end8;) {
                unsigned int ch = 0;
                const int n = ImTextCharFromUtf8(&ch, c, end8);
                if (n <= 0) break;
                c += n;
                ++letters;
            }
            const float crest = f * 1.5f - 0.25f;  // where the wave is (0..1 across the word)
            float lx = x;
            int i = 0;
            for (const char* c = text; c < end8; ++i) {
                unsigned int ch = 0;
                const int n = ImTextCharFromUtf8(&ch, c, end8);
                if (n <= 0) break;
                const float cw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, c, c + n).x;
                const float at = (static_cast<float>(i) + 0.5f) / static_cast<float>(std::max(1, letters));
                const float bump = std::exp(-((at - crest) * (at - crest)) / 0.035f) * held;
                const ImVec2 lp(lx, std::round(pos.y + dy - S(4.0f) * bump));
                const float lf = std::clamp((ww * f - (lx - x)) / std::max(1.0f, cw), 0.0f, 1.0f);
                char one[8] = {};
                std::memcpy(one, c, static_cast<std::size_t>(std::min(n, 7)));
                FillText(d, font, size, lp, height, one, p.text, alpha, 0.40f, (cw + cw * 0.8f) * lf, cw * 0.8f);
                if (bump > 0.02f && lf > 0.0f) d->AddText(font, size, lp, C(shine, 0.85f * bump * alpha), one);
                lx += cw;
                c += n;
            }
        } else {
            FillText(d, font, size, ImVec2(x, pos.y + dy), height, text, p.text, alpha, 0.40f, (ww + feather) * f, feather);
        }
        x += ww;
    }
}

// THE PAUSES (before the first line, or a long instrumental part): three dots
// that light up one by one as it goes, breathing, and shrink away at the end.
// A music note, filled (its head, its stem, its flag).
constexpr const char* kNoteFilled = "F M11 3.5L18.5 6V9.4L13.4 7.7V17.2A3.8 3.5 0 1 1 11 13.9Z";

void DrawInterlude(ImDrawList* d, ImVec2 c, float size, float progress, float alpha) {
    const Palette& p = P();
    const float now = static_cast<float>(ImGui::GetTime());
    const float out = std::clamp((1.0f - progress) / 0.06f, 0.0f, 1.0f);  // (it fades in the last moment)
    const float in = std::clamp(progress / 0.04f, 0.0f, 1.0f);
    const float a = alpha * EaseOutCubic(std::min(out, in));
    const float icon = std::round(size * 1.15f);  // (a fixed size: the icon stays sharp)
    const float breathe = 0.5f + 0.5f * std::sin(now * 2.4f);
    // its light, breathing (and with the bass)
    SoftLight(d, c, icon * 1.1f, p.accent, (0.25f + 0.25f * breathe + 0.3f * g_bassSmooth) * a);
    // the note, dim, and the part already "loaded" bright, rising from the bottom
    icons::Draw(d, kNoteFilled, c, icon, C(p.text, 0.28f * a));
    const float top = c.y + icon * 0.5f - icon * progress;
    d->PushClipRect(ImVec2(c.x - icon, top), ImVec2(c.x + icon, c.y + icon), true);
    icons::Draw(d, kNoteFilled, c, icon, C(p.text, a));
    d->PopClipRect();
}

const lyrics::Line* LineOf(int k, int* end);  // (below)

// Is it a pause now? (before the first line, or an empty line between parts)
// Gives how far it went (0..1).
bool InterludeAt(int ms, float* progress) {
    const int count = static_cast<int>(g_lyrics.lines.size());
    if (count == 0) return false;
    auto blank = [](const std::string& s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; };
    if (g_lyricLine < 0) {
        const int first = g_lyrics.lines[0].ms;
        if (first < 2500) return false;
        *progress = std::clamp(static_cast<float>(ms) / static_cast<float>(first), 0.0f, 1.0f);
        return true;
    }
    int end = 0;
    const lyrics::Line* line = LineOf(g_lyricLine, &end);
    if (line == nullptr || !blank(line->text) || end - line->ms < 2500) return false;
    *progress = std::clamp(static_cast<float>(ms - line->ms) / static_cast<float>(end - line->ms), 0.0f, 1.0f);
    return true;
}

// The line "k" (or nothing), with its end time.
const lyrics::Line* LineOf(int k, int* end) {
    const int count = static_cast<int>(g_lyrics.lines.size());
    if (k < 0 || k >= count) return nullptr;
    const lyrics::Line& line = g_lyrics.lines[static_cast<std::size_t>(k)];
    if (end != nullptr) *end = k + 1 < count ? g_lyrics.lines[static_cast<std::size_t>(k + 1)].ms : line.ms + 6000;
    return &line;
}

// The height the short lyrics take under the song's name.
float CompactLyricsHeight() {
    if (!g_homeLyrics) return 0.0f;
    return (g_fonts.line->FontSize + S(4.0f) + g_fonts.regular->FontSize + S(18.0f)) * EaseOutCubic(g_lyricsShow);
}

// THE SHORT LYRICS: under the song's name, the line being sung (filling) and
// the next one, dim. When the line changes, the next one rises into place.
void DrawCompactLyrics(ImDrawList* d, float x, float y, float w, const media::Track& t, float alpha) {
    const Palette& p = P();
    if (!g_homeLyrics) return;
    alpha *= g_lyricsShow * (1.0f - g_lyricsViewAnim);
    if (alpha < 0.01f || !t.present || g_lyrics.trackId != t.trackId) return;
    ImFont* big = g_fonts.line;
    ImFont* nextFont = g_fonts.regular;  // (the next line)
    const float lineH = big->FontSize + S(4.0f);
    if (g_lyrics.status == lyrics::Status::Searching) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
        Text(d, nextFont, ImVec2(x, y + S(4.0f)), C(p.text, (0.35f + 0.25f * pulse) * alpha), T("lyrics.searching"));
        return;
    }
    if (g_lyrics.status == lyrics::Status::Instrumental) {
        Text(d, nextFont, ImVec2(x, y + S(4.0f)), C(p.text, 0.55f * alpha), T("lyrics.instrumental"));
        return;
    }
    if (!HasLyricLines(t)) return;
    const int ms = LyricsMs(t);
    const float move = 1.0f - EaseOutCubic(g_lyricScroll);  // 1 = just changed
    d->PushClipRect(ImVec2(x - S(8.0f), y - S(6.0f)), ImVec2(x + w + S(8.0f), y + lineH + nextFont->FontSize + S(8.0f)), true);
    // the line that just ended, going up and away
    int end = 0;
    if (move > 0.0f) {
        if (const lyrics::Line* old = LineOf(g_lyricLine - 1, nullptr)) {
            const float size = big->FontSize * std::min(1.0f, w / std::max(1.0f, TextSize(big, old->text.c_str()).x));
            d->AddText(big, size, ImVec2(x, y - lineH * (1.0f - move)), C(p.text, alpha * move * 0.8f), old->text.c_str());
        }
    }
    // the line being sung (or, in a pause, the three dots)
    float pause = 0.0f;
    if (InterludeAt(ms, &pause)) {
        DrawInterlude(d, ImVec2(x + big->FontSize * 0.55f, y + lineH * 0.5f), big->FontSize, pause, alpha);
    } else if (const lyrics::Line* line = LineOf(g_lyricLine, &end)) {
        float size = big->FontSize;
        ImVec2 ts = big->CalcTextSizeA(size, FLT_MAX, 0.0f, line->text.c_str());
        if (ts.x > w - S(8.0f)) {  // too long: smaller, so it fits (with room to grow a held note)
            size *= (w - S(8.0f)) / ts.x;
            ts = big->CalcTextSizeA(size, FLT_MAX, 0.0f, line->text.c_str());
        }
        const float ly = y + S(2.0f) + (lineH * 0.7f) * move;
        DrawSungLine(d, big, size, ImVec2(x, ly + (big->FontSize - size) * 0.5f), ts.y, *line, end, ms, alpha * (1.0f - 0.6f * move),
                     S(2.0f));
    }
    // the next one
    if (const lyrics::Line* next = LineOf(g_lyricLine + 1, nullptr)) {
        TextFit(d, nextFont, ImVec2(x, y + lineH + S(2.0f) + S(10.0f) * move), C(p.text, 0.45f * alpha * (1.0f - 0.5f * move)),
                next->text.c_str(), w);
    }
    d->PopClipRect();
}

// THE LYRICS VIEW: big lines in the middle, the one being sung filling, the
// others dim and smaller, moving up softly when it changes.
void DrawLyricsLines(ImDrawList* d, ImVec2 min, ImVec2 max, const media::Track& t, float alpha) {
    const Palette& p = P();
    if (alpha < 0.01f) return;
    ImFont* font = g_fonts.lyric;
    const float W = max.x - min.x;
    const float cx = min.x + W * 0.5f;
    const float cy = (min.y + max.y) * 0.5f;
    auto centered = [&](const char* text, float a) {
        const ImVec2 ts = TextSize(g_fonts.title, text);
        Text(d, g_fonts.title, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), C(p.text, a), text);
    };
    if (g_lyrics.trackId != t.trackId || g_lyrics.status == lyrics::Status::Searching) {
        centered(T("lyrics.searching"), 0.5f * alpha);
        return;
    }
    if (g_lyrics.status == lyrics::Status::Instrumental) {
        centered(T("lyrics.instrumental"), 0.6f * alpha);
        return;
    }
    if (!HasLyricLines(t)) {
        centered(g_lyrics.status == lyrics::Status::Found ? T("lyrics.unsynced") : T("lyrics.not_found"), 0.5f * alpha);
        return;
    }
    const int ms = LyricsMs(t);
    const float big = font->FontSize;
    const float step = big * 1.55f;
    d->PushClipRect(min, max, true);
    // the pause dots, in the current line's place
    {
        float pause = 0.0f;
        if (InterludeAt(ms, &pause)) {
            const float in = EaseOutBack(g_lyricScroll);
            DrawInterlude(d, ImVec2(cx, cy + (1.0f - in) * step), big, pause, alpha);
        }
    }
    for (int k = g_lyricLine - 3; k <= g_lyricLine + 4; ++k) {
        int end = 0;
        const lyrics::Line* line = LineOf(k, &end);
        if (line == nullptr || line->text.find_first_not_of(" ") == std::string::npos) continue;
        // (each line follows a moment after the one above: a soft cascade)
        const float delay = 0.07f * static_cast<float>(std::clamp(k - g_lyricLine + 1, 0, 5));
        const float local = std::clamp((g_lyricScroll - delay) / 0.65f, 0.0f, 1.0f);
        const float scroll = 1.0f - EaseOutBack(local);  // 1 = still where the old line was
        const float r = static_cast<float>(k - g_lyricLine) + scroll;  // 0 = the current one's place
        const float near1 = std::clamp(1.0f - std::fabs(r), 0.0f, 1.0f);
        float size = big * (0.72f + 0.28f * near1);
        const float a = alpha * std::clamp(1.0f - (std::fabs(r) - 1.0f) * 0.35f, 0.0f, 1.0f);
        if (a < 0.01f) continue;
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, line->text.c_str());
        if (ts.x > W * 0.94f) {
            size *= W * 0.94f / ts.x;
            ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, line->text.c_str());
        }
        const ImVec2 pos(cx - ts.x * 0.5f, cy + r * step - ts.y * 0.5f);
        if (k == g_lyricLine) {
            DrawSungLine(d, font, size, pos, ts.y, *line, end, ms, a, S(4.0f));
        } else {
            // the farther from the line being sung, the more blurred (like a
            // camera focused on it): sharp -> a little blur -> more blur
            const float base = (k < g_lyricLine ? 0.30f : 0.42f) * a;
            const float blur = std::clamp(std::fabs(r) - 0.6f, 0.0f, 2.0f);
            const char* tx = line->text.c_str();
            if (blur < 1.0f) {
                if (blur < 0.99f) d->AddText(font, size, pos, C(p.text, base * (1.0f - blur)), tx);
                BlurText(d, font, size, pos, C(p.text, base * blur), tx, nullptr, 0);
            } else {
                BlurText(d, font, size, pos, C(p.text, base * (2.0f - blur)), tx, nullptr, 0);
                BlurText(d, font, size, pos, C(p.text, base * (blur - 1.0f) * 1.2f), tx, nullptr, 1);
            }
        }
    }
    d->PopClipRect();
}

// --- The player's extras: favorites, small buttons, the volume, the options --------------

// YOUR FAVORITES: the heart on the album art saves the song in Luma's own
// list, on your PC (%APPDATA%\Luma\favorites.json). Windows doesn't let any
// program "like" a song inside Spotify; that needs connecting your Spotify
// account (a next part).
struct Favorite {
    std::string title, artist, album;
    long long added = 0;  // when (seconds since 1970)
};
std::vector<Favorite> g_favorites;
bool g_favoritesLoaded = false;

void LoadFavorites() {
    g_favoritesLoaded = true;
    const nlohmann::json json = ReadJson(DataDir() / L"favorites.json");
    const auto list = json.find("songs");
    if (list == json.end() || !list->is_array()) return;
    for (const auto& s : *list) {
        if (!s.is_object()) continue;
        Favorite f;
        f.title = s.value("title", std::string());
        f.artist = s.value("artist", std::string());
        f.album = s.value("album", std::string());
        f.added = s.value("added", 0LL);
        if (!f.title.empty()) g_favorites.push_back(std::move(f));
    }
}

void SaveFavorites() {
    const fs::path dir = DataDir();
    if (dir.empty()) return;
    nlohmann::json songs = nlohmann::json::array();
    for (const Favorite& f : g_favorites) {
        songs.push_back({{"title", f.title}, {"artist", f.artist}, {"album", f.album}, {"added", f.added}});
    }
    nlohmann::json json;
    json["songs"] = std::move(songs);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / L"favorites.json";
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << json.dump(2);
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

int FindFavorite(const media::Track& t) {
    if (!g_favoritesLoaded) LoadFavorites();
    for (std::size_t i = 0; i < g_favorites.size(); ++i) {
        if (g_favorites[i].title == t.title && g_favorites[i].artist == t.artist) return static_cast<int>(i);
    }
    return -1;
}

void ToggleFavorite(const media::Track& t) {
    const int i = FindFavorite(t);
    if (i >= 0) {
        g_favorites.erase(g_favorites.begin() + i);
    } else {
        g_favorites.push_back(Favorite{t.title, t.artist, t.album, static_cast<long long>(std::time(nullptr))});
    }
    SaveFavorites();
}

// A small round icon button (shuffle, repeat...): lit in the accent when on.
bool IconToggle(ImDrawList* d, const char* id, ImVec2 c, const char* icon, float size, bool on, bool enabled = true) {
    const Palette& p = P();
    const float r = size * 0.95f;
    float hover = 0.0f;
    const bool clicked = Hit(id, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), &hover, enabled);
    const float lit = Anim(ImGui::GetID(id) + 9, on, 10.0f);
    const float press = Anim(ImGui::GetID(id) + 10, ImGui::IsItemActive() && enabled, 22.0f);
    if (hover > 0.0f) d->AddCircleFilled(c, r * (1.0f - 0.08f * press), C(ImVec4(1, 1, 1, 1), 0.10f * hover), 32);
    const float a = enabled ? 1.0f : 0.3f;
    icons::Draw(d, icon, c, size, C(Mix(p.text, p.accent, lit), a * (0.70f + 0.30f * std::max(hover, lit))), S(1.8f));
    if (lit > 0.01f) d->AddCircleFilled(ImVec2(c.x, c.y + size * 0.85f), S(2.0f) * lit, C(p.accent, a * lit), 12);  // the little dot under it
    return clicked && enabled;
}

// THE PROGRESS BAR: it grows when the mouse is near and its knob appears; the
// filled part glides (no jumps); click or drag it: while dragging, a bubble
// shows the time, and the song moves there when you let go.
void SeekBar(ImDrawList* d, const char* id, float x0, float x1, float y, const media::Track& t, float alpha, bool times,
             void (*seekFn)(double) = nullptr) {
    const Palette& p = P();
    if (t.duration <= 0.0) return;
    const float w = std::max(1.0f, x1 - x0);
    float hover = 0.0f;
    Hit(id, ImVec2(x0 - S(6.0f), y - S(12.0f)), ImVec2(x1 + S(6.0f), y + S(12.0f)), &hover, t.canSeek);
    const bool active = ImGui::IsItemActive() && t.canSeek;
    const ImGuiID gid = ImGui::GetID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    float& shown = *st->GetFloatRef(gid + 21, -1.0f);
    float& wasActive = *st->GetFloatRef(gid + 22, 0.0f);
    float& holdUntil = *st->GetFloatRef(gid + 23, 0.0f);  // (after a seek: wait for the app to get there)
    const float real = static_cast<float>(std::clamp(t.position / t.duration, 0.0, 1.0));
    const float mouse = std::clamp((ImGui::GetIO().MousePos.x - x0) / w, 0.0f, 1.0f);
    const float now = static_cast<float>(ImGui::GetTime());
    if (active) {
        shown = mouse;
    } else {
        if (wasActive > 0.5f) {
            (seekFn != nullptr ? seekFn : media::Seek)(t.duration * shown);
            holdUntil = now + 0.8f;
        }
        if (shown < 0.0f) shown = real;
        if (now > holdUntil || std::fabs(real - shown) < 0.01f) shown += (real - shown) * std::min(1.0f, Dt() * 10.0f);
    }
    wasActive = active ? 1.0f : 0.0f;
    const float press = Anim(gid + 5, active, 16.0f);
    const float grow = std::max(hover, press);
    const float th = S(4.0f) + S(3.0f) * grow;
    d->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x1, y + th * 0.5f), C(ImVec4(1, 1, 1, 1), 0.16f * alpha), th * 0.5f);
    // where the mouse points (a lighter part, while hovering)
    if (hover > 0.01f && !active) {
        d->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x0 + w * mouse, y + th * 0.5f), C(ImVec4(1, 1, 1, 1), 0.10f * hover * alpha), th * 0.5f);
    }
    const float fx = x0 + w * shown;
    d->AddRectFilled(ImVec2(x0, y - th * 1.4f), ImVec2(fx, y + th * 1.4f), C(p.accent, 0.12f * alpha), th);
    d->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(fx, y + th * 0.5f), C(p.accent, alpha), th * 0.5f);
    const float knob = S(3.0f) + S(4.0f) * grow + S(2.0f) * press;
    if (press > 0.01f) d->AddCircleFilled(ImVec2(fx, y), knob + S(7.0f) * press, C(p.accent, 0.20f * press * alpha), 24);
    d->AddCircleFilled(ImVec2(fx, y), knob, C(p.text, alpha), 24);
    char a[16], b[16];
    FormatTime(t.duration * shown, a, sizeof(a));
    FormatTime(t.duration, b, sizeof(b));
    if (press > 0.01f) {  // the time bubble above the knob
        const ImVec2 ts = TextSize(g_fonts.semibold, a);
        const float bw = ts.x + S(18.0f), bh = ts.y + S(8.0f);
        const float rise = S(6.0f) * (1.0f - press);
        const ImVec2 bmin(fx - bw * 0.5f, y - S(16.0f) - bh + rise), bmax(fx + bw * 0.5f, y - S(16.0f) + rise);
        d->AddRectFilled(bmin, bmax, C(Mix(p.card, ImVec4(0, 0, 0, 1), 0.2f), 0.92f * press * alpha), bh * 0.5f);
        Text(d, g_fonts.semibold, ImVec2(bmin.x + S(9.0f), bmin.y + S(4.0f)), C(p.text, press * alpha), a);
    }
    if (times) {
        Text(d, g_fonts.tiny, ImVec2(x0, y + S(10.0f)), C(p.text, 0.6f * alpha), a);
        Text(d, g_fonts.tiny, ImVec2(x1 - TextSize(g_fonts.tiny, b).x, y + S(10.0f)), C(p.text, 0.6f * alpha), b);
    }
}

// The lyrics view's progress: a thin line that looks as it always did; click
// or drag it to move the song (it seeks when you let go).
void ThinSeek(ImDrawList* d, const char* id, float x0, float x1, float y, const media::Track& t, float alpha,
              void (*seekFn)(double) = nullptr) {
    const Palette& p = P();
    if (t.duration <= 0.0) return;
    const float w = std::max(1.0f, x1 - x0);
    Hit(id, ImVec2(x0 - S(4.0f), y - S(9.0f)), ImVec2(x1 + S(4.0f), y + S(9.0f)), nullptr, t.canSeek);
    const bool active = ImGui::IsItemActive() && t.canSeek;
    const ImGuiID gid = ImGui::GetID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    float& dragged = *st->GetFloatRef(gid + 21, -1.0f);  // where it's being dragged (-1 = it isn't)
    float& seekedAt = *st->GetFloatRef(gid + 23, 0.0f);  // when it was let go (0 = not yet)
    const float now = static_cast<float>(ImGui::GetTime());
    float frac = static_cast<float>(std::clamp(t.position / t.duration, 0.0, 1.0));
    if (active) {
        dragged = std::clamp((ImGui::GetIO().MousePos.x - x0) / w, 0.0f, 1.0f);
        seekedAt = 0.0f;
    } else if (dragged >= 0.0f && seekedAt == 0.0f) {
        (seekFn != nullptr ? seekFn : media::Seek)(t.duration * dragged);  // let go: the song moves there
        seekedAt = now;
    } else if (seekedAt > 0.0f && now - seekedAt > 0.8f) {
        dragged = -1.0f;  // (by now the app is there: back to its real position)
        seekedAt = 0.0f;
    }
    if (dragged >= 0.0f) frac = dragged;
    d->AddRectFilled(ImVec2(x0, y - S(1.5f)), ImVec2(x1, y + S(1.5f)), C(ImVec4(1, 1, 1, 1), 0.15f * alpha), S(1.5f));
    d->AddRectFilled(ImVec2(x0, y - S(1.5f)), ImVec2(x0 + w * frac, y + S(1.5f)), C(p.accent, alpha), S(1.5f));
}

// The app's volume: an icon and a thin bar you drag.
void VolumeBar(ImDrawList* d, ImVec2 left, float width, const media::Track& t, float alpha) {
    const Palette& p = P();
    if (t.volume < 0.0f) return;  // (its audio wasn't found)
    icons::Draw(d, icons::kVolume, left, S(17.0f), C(p.text, 0.75f * alpha), S(1.7f));
    const float x0 = left.x + S(18.0f), x1 = x0 + width;
    float hover = 0.0f;
    Hit("##volume", ImVec2(x0 - S(6.0f), left.y - S(10.0f)), ImVec2(x1 + S(6.0f), left.y + S(10.0f)), &hover);
    static float shown = -1.0f;
    const float press = Anim(ImGui::GetID("##volume") + 4, ImGui::IsItemActive(), 16.0f);
    if (ImGui::IsItemActive()) {
        const float v = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, width), 0.0f, 1.0f);
        if (std::fabs(v - shown) > 0.005f) media::SetVolume(v);
        shown = v;
    } else if (shown < 0.0f) {
        shown = t.volume;
    } else {
        shown += (t.volume - shown) * std::min(1.0f, Dt() * 12.0f);  // (changed elsewhere, or by the keys: it glides)
    }
    hover = std::max(hover, press);
    const float th = S(4.0f) + S(2.0f) * hover;
    d->AddRectFilled(ImVec2(x0, left.y - th * 0.5f), ImVec2(x1, left.y + th * 0.5f), C(ImVec4(1, 1, 1, 1), 0.16f * alpha), th);
    d->AddRectFilled(ImVec2(x0, left.y - th * 0.5f), ImVec2(x0 + width * shown, left.y + th * 0.5f), C(p.text, 0.85f * alpha), th);
    if (hover > 0.0f) d->AddCircleFilled(ImVec2(x0 + width * shown, left.y), S(5.0f) * hover, C(p.text, alpha), 16);
}

// THE OPTIONS (the "..." button): the lyrics' timing (shared with the game),
// copy the song's name, open Spotify.
bool g_optionsOpen = false;
ImVec2 g_optionsAnchor;  // the "..." button's center (it opens above it)
float g_copiedAt = -10.0f;

float g_optionsAnim = 0.0f;  // 0..1: the menu opening

void DrawOptionsMenuBody(ImDrawList* d, const media::Track& t);

void DrawOptionsMenu(ImDrawList* d, const media::Track& t) {
    if (!t.present) g_optionsOpen = false;
    g_optionsAnim += ((g_optionsOpen ? 1.0f : 0.0f) - g_optionsAnim) * std::min(1.0f, Dt() * 16.0f);
    if (g_optionsAnim < 0.01f) return;
    const int start = d->VtxBuffer.Size;
    DrawOptionsMenuBody(d, t);
    const float e = EaseOutCubic(g_optionsAnim);
    AnimateSince(d, start, e, ImVec2(0.0f, (1.0f - e) * S(10.0f)));
}

void DrawOptionsMenuBody(ImDrawList* d, const media::Track& t) {
    const Palette& p = P();
    const bool spotify = std::strstr(t.app, "Spotify") != nullptr;
    const float itemH = S(40.0f);
    const int rows = 2 + (spotify ? 1 : 0);
    const float width = S(300.0f);
    const float keysH = TextWrapHeight(g_fonts.tiny, T("key.help"), width - S(28.0f)) + S(16.0f);
    const float height = itemH * static_cast<float>(rows) + S(12.0f) + keysH;
    const ImVec2 max(g_optionsAnchor.x + S(18.0f), g_optionsAnchor.y - S(24.0f));
    const ImVec2 min(max.x - width, max.y - height);
    d->AddRectFilled(ImVec2(min.x, min.y + S(6.0f)), ImVec2(max.x, max.y + S(8.0f)), Col(ImVec4(0, 0, 0, 1), 0.25f), S(14.0f));
    d->AddRectFilled(min, max, Col(Mix(p.card, ImVec4(1, 1, 1, 1), 0.04f), 0.97f), S(14.0f));
    d->AddRect(min, max, Col(ImVec4(1, 1, 1, 1), 0.12f), S(14.0f));
    float y = min.y + S(6.0f);
    bool done = false;
    // 1. The lyrics' timing: - value +
    {
        const ImVec2 imin(min.x + S(6.0f), y), imax(max.x - S(6.0f), y + itemH);
        const float cy = (imin.y + imax.y) * 0.5f;
        icons::Draw(d, icons::kClock, ImVec2(imin.x + S(18.0f), cy), S(17.0f), Col(p.textDim), S(1.6f));
        Text(d, g_fonts.semibold, ImVec2(imin.x + S(38.0f), cy - g_fonts.semibold->FontSize * 0.5f), Col(p.text), T("opt.offset"));
        const float off = flowcfg::g_state.offset;
        char value[16];
        std::snprintf(value, sizeof(value), "%+.1f s", off);
        const ImVec2 vs = TextSize(g_fonts.tiny, value);
        const float plusX = imax.x - S(16.0f), minusX = plusX - S(36.0f) - vs.x - S(10.0f);
        Text(d, g_fonts.tiny, ImVec2((minusX + plusX) * 0.5f - vs.x * 0.5f, cy - vs.y * 0.5f), Col(p.text, 0.85f), value);
        const float savedFade = g_fade;
        g_fade = 1.0f;
        // (+ = the lyrics come later, - = earlier: the same as the game's "Offset")
        if (IconToggle(d, "##off_minus", ImVec2(minusX, cy), icons::kMinus, S(14.0f), false)) flowcfg::SetOffset(std::clamp(off - 0.1f, -5.0f, 5.0f));
        if (IconToggle(d, "##off_plus", ImVec2(plusX, cy), icons::kPlus, S(14.0f), false)) flowcfg::SetOffset(std::clamp(off + 0.1f, -5.0f, 5.0f));
        g_fade = savedFade;
        y += itemH;
    }
    auto item = [&](const char* id, const char* icon, const char* label) {
        const ImVec2 imin(min.x + S(6.0f), y), imax(max.x - S(6.0f), y + itemH);
        float hover = 0.0f;
        const bool clicked = Hit(id, imin, imax, &hover);
        d->AddRectFilled(imin, imax, Col(p.text, 0.07f * hover), S(9.0f));
        icons::Draw(d, icon, ImVec2(imin.x + S(18.0f), (imin.y + imax.y) * 0.5f), S(17.0f), Col(p.textDim), S(1.6f));
        Text(d, g_fonts.semibold, ImVec2(imin.x + S(38.0f), (imin.y + imax.y - g_fonts.semibold->FontSize) * 0.5f), Col(p.text), label);
        y += itemH;
        return clicked;
    };
    const bool copied = ImGui::GetTime() - g_copiedAt < 1.5;
    if (item("##opt_copy", copied ? icons::kCheck : icons::kCopy, copied ? T("opt.copied") : T("opt.copy"))) {
        char text[340];
        std::snprintf(text, sizeof(text), "%s - %s", t.title, t.artist);
        ImGui::SetClipboardText(text);
        g_copiedAt = static_cast<float>(ImGui::GetTime());
    }
    if (spotify && item("##opt_spotify", icons::kAppWindow, T("opt.open_spotify"))) {
        ShellExecuteW(nullptr, L"open", L"spotify:", nullptr, nullptr, SW_SHOWNORMAL);
        done = true;
    }
    d->AddLine(ImVec2(min.x + S(14.0f), y + S(4.0f)), ImVec2(max.x - S(14.0f), y + S(4.0f)), Col(ImVec4(1, 1, 1, 1), 0.08f));
    TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(14.0f), y + S(10.0f)), Col(p.textDim), T("key.help"), width - S(28.0f));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool inside = mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y;
    if (done || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inside && !ImGui::IsAnyItemHovered()) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_optionsOpen = false;
    }
}

// The filled heart's color: the album art's (the launcher's accent), made a
// little brighter when the cover is very dark, so it shows on the dark glass.
ImVec4 HeartColor() {
    ImVec4 c = P().accent;
    const float light = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    if (light < 0.45f) {
        const float k = (0.45f - light) / 0.45f * 0.6f;
        c = ImVec4(c.x + (1.0f - c.x) * k, c.y + (1.0f - c.y) * k, c.z + (1.0f - c.z) * k, 1.0f);
    }
    return c;
}

// The heart, in the album art's corner (glass, filled when it's a favorite;
// it pops when you press it).
void DrawArtHeart(ImDrawList* d, ImVec2 artC, float artSize, const media::Track& t, float alpha) {
    const float half = artSize * 0.5f;
    const float r = S(19.0f);
    const ImVec2 c(artC.x + half - r - S(10.0f), artC.y + half - r - S(10.0f));
    float hover = 0.0f;
    // (when Spotify is connected and plays this song: your Liked songs on
    // Spotify, in Spotify's green; otherwise Luma's own favorites)
    spotify::Player sp;
    const bool viaSpotify = SpotifyDrivesHome(t, sp) && sp.likedKnown;
    const bool fav = viaSpotify ? sp.liked : FindFavorite(t) >= 0;
    static float pop = 0.0f;
    if (Hit("##heart", ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), &hover)) {
        if (viaSpotify) {
            spotify::PlayerLike(!sp.liked);
        } else {
            ToggleFavorite(t);
        }
        pop = 1.0f;
    }
    pop = std::max(0.0f, pop - Dt() * 4.0f);
    const float grow = 1.0f + 0.25f * std::sin(pop * 3.14159f);
    d->AddCircleFilled(c, r * (1.0f + 0.06f * hover), C(ImVec4(0.05f, 0.05f, 0.06f, 1.0f), (0.45f + 0.2f * hover) * alpha), 32);
    d->AddCircle(c, r * (1.0f + 0.06f * hover), C(ImVec4(1, 1, 1, 1), 0.15f * alpha), 32, 1.0f);
    // (filled: in the album art's own color, like the rest of the launcher)
    icons::Draw(d, fav ? icons::kHeartFilled : icons::kHeart, c, S(18.0f) * grow, C(fav ? HeartColor() : ImVec4(1, 1, 1, 1), alpha), S(1.8f));
}

// --- 3. The Lyrics sheet ------------------------------------------------------------------

constexpr const char* kStyleKeys[6][2] = {
    {"style.karaoke", "style.karaoke_d"}, {"style.neon", "style.neon_d"},   {"style.party", "style.party_d"},
    {"style.dream", "style.dream_d"},     {"style.fire", "style.fire_d"},   {"style.minimal", "style.minimal_d"},
};

// A hue (0..1) as a color.
ImVec4 Hue(float h) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h - std::floor(h), 0.75f, 1.0f, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

// A small random number from two integers (the same every time: particles
// that don't need to be stored).
float Hash(int a, int b) {
    unsigned int h = static_cast<unsigned int>(a) * 374761393u + static_cast<unsigned int>(b) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

// THE PREVIEW of a style: a text ("LUMA" on the cards; the line being sung on
// the lyrics section's stage) drawn the way the style draws lyrics in the game
// (its colors, its fill, its light, its shadow and border, its particles),
// animated. An approximation drawn here, not the game's own code (that one
// draws in the 3D world), but with the same choices. "progress" 0..1 = how
// much is sung (-1: a loop). "maxWidth" > 0: a long line is made smaller.
void DrawFlowLook(ImDrawList* d, ImVec2 center, float height, const luma::flowstyles::FlowLook& k, float alpha, float seed,
                  const char* text, float progress, float maxWidth) {
    const Palette& p = P();
    const float now = static_cast<float>(ImGui::GetTime()) + seed;
    // the letters, one by one (UTF-8)
    constexpr int kMaxLetters = 96;
    const char* starts[kMaxLetters + 1];
    int n = 0;
    bool latin = true;
    const char* end = text + std::strlen(text);
    const char* cur = text;
    while (cur < end && n < kMaxLetters) {
        unsigned int c = 0;
        const int len = ImTextCharFromUtf8(&c, cur, end);
        if (len <= 0) break;
        if (c > 0x24F) latin = false;
        starts[n++] = cur;
        cur += len;
    }
    starts[n] = cur;
    if (n == 0) return;
    // Minecraft's pixel letters; another alphabet: a font that has it
    ImFont* font = latin ? g_fonts.pixel : g_fonts.line;
    // (pixel letters: a whole multiple of their size, so the pixels stay square)
    float size = latin ? std::max(1.0f, std::floor(height * 0.42f / std::max(1.0f, font->FontSize))) * font->FontSize : height * 0.42f;
    float widths[kMaxLetters], total = 0.0f;
    float gap = n <= 6 ? size * 0.08f : 0.0f;  // ("LUMA" spaced; a real line, as written)
    for (int i = 0; i < n; ++i) {
        widths[i] = font->CalcTextSizeA(size, FLT_MAX, 0.0f, starts[i], starts[i + 1]).x;
        total += widths[i] + (i < n - 1 ? gap : 0.0f);
    }
    if (maxWidth > 0.0f && total > maxWidth) {
        const float k2 = maxWidth / total;
        size *= k2;
        gap *= k2;
        total = maxWidth;
        for (int i = 0; i < n; ++i) widths[i] *= k2;
    }
    const float x0 = center.x - total * 0.5f;
    const float y0 = center.y - size * 0.5f;
    // The fill: what's sung, or a loop of 2.6 s (it fills, stays a moment, starts again).
    const float cycle = std::fmod(now * k.speed, 2.6f) / 2.0f;
    const float sweep = (progress >= 0.0f ? progress : std::clamp(cycle, 0.0f, 1.0f)) * total;
    // each letter's color
    const ImVec4 albumA = p.accent;
    const ImVec4 albumB = Mix(p.accent, ImVec4(1, 1, 1, 1), 0.45f);
    auto letterColor = [&](int i) {
        const float f = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        switch (k.colorMode) {
            case 1: return albumA;
            case 2: return Mix(albumA, albumB, f);
            case 3: return Mix(ImVec4(k.color[0], k.color[1], k.color[2], 1), ImVec4(k.color2[0], k.color2[1], k.color2[2], 1), f);
            case 4: return Hue(f * 0.75f + now * 0.15f);
            default: return ImVec4(k.color[0], k.color[1], k.color[2], 1);
        }
    };
    const ImVec4 mainColor = letterColor(n / 2);
    // its light (soft, behind the letters)
    if (k.glow != 0) {
        float strength = 0.75f * k.glowStrength;
        if (k.glow == 5) strength *= 0.6f + 0.4f * (0.5f + 0.5f * std::sin(now * 4.0f));  // pulse
        const ImVec4 gc = k.glowColor == 0 ? mainColor : (k.glowColor == 1 ? p.accent : mainColor);
        const float wide = k.glow == 2 ? 1.5f : 1.0f;
        if (k.glowWhere == 0 && k.fill != 0) {
            // only on the part being sung
            const float gx = x0 + std::min(sweep, total);
            SoftLightOval(d, ImVec2(gx - size * 0.4f, center.y), size * 1.1f * wide, size * 0.9f * wide, gc, strength * alpha);
        } else {
            SoftLightOval(d, center, total * 0.62f * wide + size * 0.4f, size * 0.95f * wide, gc, strength * alpha);
        }
    }
    // the letters
    float x = x0;
    for (int i = 0; i < n; ++i) {
        float dy = 0.0f;
        if (k.motion != 0) dy = std::sin(now * 3.0f * k.speed + static_cast<float>(i) * 0.9f) * size * 0.06f;
        if (k.appear == 3) dy += std::sin(now * 1.4f) * size * 0.05f;  // floating
        const ImVec2 pos(std::round(x), std::round(y0 + dy));
        const ImVec4 col = letterColor(i);
        const float px = std::max(1.0f, std::round(size / std::max(1.0f, font->FontSize) * 1.5f));
        // shadow: dark, colored or 3D
        if (k.shadow) {
            if (k.shadowStyle == 2) {
                for (int s = 4; s >= 1; --s) {
                    d->AddText(font, size, ImVec2(pos.x + px * s * 0.5f, pos.y + px * s * 0.5f), C(Mix(col, ImVec4(0, 0, 0, 1), 0.65f), alpha), starts[i], starts[i + 1]);
                }
            } else {
                const ImVec4 sc = k.shadowStyle == 1 ? col : ImVec4(0, 0, 0, 1);
                d->AddText(font, size, ImVec2(pos.x + px * 1.5f, pos.y + px * 1.5f), C(sc, (k.shadowStyle == 1 ? 0.45f : 0.6f) * alpha), starts[i], starts[i + 1]);
            }
        }
        // border
        if (k.outline) {
            const ImU32 oc = C(ImVec4(luma::flowstyles::kDefaultOutlineColor[0], luma::flowstyles::kDefaultOutlineColor[1],
                                      luma::flowstyles::kDefaultOutlineColor[2], 1), luma::flowstyles::kDefaultOutlineAlpha * alpha);
            for (int o = 0; o < 4; ++o) {
                const float ox = (o == 0 ? -px : o == 1 ? px : 0.0f), oy = (o == 2 ? -px : o == 3 ? px : 0.0f);
                d->AddText(font, size, ImVec2(pos.x + ox, pos.y + oy), oc, starts[i], starts[i + 1]);
            }
        }
        // the letter: not yet sung (dim), and the sung part on top
        if (k.fill == 0) {
            d->AddText(font, size, pos, C(col, alpha), starts[i], starts[i + 1]);
        } else {
            const float unsungA = k.unsung == 0 ? 0.35f : 0.2f;
            d->AddText(font, size, pos, C(Mix(col, ImVec4(1, 1, 1, 1), 0.4f), unsungA * alpha), starts[i], starts[i + 1]);
            const float lit = std::clamp(sweep - (x - x0), 0.0f, widths[i]);
            if (lit > 0.0f) {
                d->PushClipRect(ImVec2(pos.x - px * 3, pos.y - size), ImVec2(pos.x + lit, pos.y + size * 2.0f), true);
                d->AddText(font, size, pos, C(col, alpha), starts[i], starts[i + 1]);
                d->PopClipRect();
            }
        }
        x += widths[i] + gap;
    }
    // a bright edge where it's filling (Neon's "edge")
    if (k.fill == 4 && sweep > 0.0f && sweep < total) {
        const float ex = x0 + sweep;
        d->AddRectFilled(ImVec2(ex - S(1.0f), y0), ImVec2(ex + S(1.0f), y0 + size), C(ImVec4(1, 1, 1, 1), 0.85f * alpha));
        SoftLight(d, ImVec2(ex, center.y), size * 0.6f, mainColor, 0.9f * alpha);
    }
    // the particles: each one is born, flies and fades (from its number alone)
    if (k.vfx != 0) {
        // (a long line gets more of them, so they spread along it)
        const int sparks = static_cast<int>(10 * k.vfxAmount * std::clamp(total / (size * 4.0f), 1.0f, 4.0f));
        for (int i = 0; i < sparks; ++i) {
            const float life = 1.4f + Hash(i, 7) * 0.8f;
            const float tt = now / life + Hash(i, 3);
            const int gen = static_cast<int>(std::floor(tt));
            const float f = tt - static_cast<float>(gen);
            const float sx = x0 + Hash(i, gen * 31 + 1) * total;
            float px = sx, py = center.y;
            ImVec4 pc = k.vfxColor == 2 ? Hue(Hash(i, gen)) : (k.vfxColor == 1 ? p.accent : mainColor);
            float r = S(1.6f);
            switch (k.vfx) {
                case 1:  // sparks: out and down, fast
                    px += (Hash(i, gen * 5) - 0.5f) * size * 1.6f * f;
                    py += -size * 0.6f * f + size * 1.4f * f * f;
                    pc = Mix(pc, ImVec4(1, 1, 1, 1), 0.5f);
                    break;
                case 2:  // stardust: slowly up, twinkling
                    px += std::sin(f * 6.0f + static_cast<float>(i)) * size * 0.15f;
                    py += size * 0.5f - size * 1.3f * f;
                    r = S(1.2f) + S(1.0f) * (0.5f + 0.5f * std::sin(now * 9.0f + static_cast<float>(i)));
                    break;
                case 3:  // confetti: falls, turning
                    py = center.y - size * 1.0f + size * 2.2f * f;
                    px += std::sin(f * 9.0f + static_cast<float>(i)) * size * 0.25f;
                    r = S(2.0f);
                    break;
                default:  // embers: rise, from orange to dark
                    px += std::sin(f * 4.0f + static_cast<float>(i) * 2.0f) * size * 0.2f;
                    py += size * 0.4f - size * 1.5f * f;
                    pc = Mix(ImVec4(1.0f, 0.75f, 0.25f, 1), ImVec4(0.9f, 0.25f, 0.05f, 1), f);
                    break;
            }
            const float a = std::sin(f * 3.14159f) * alpha;
            if (k.vfx == 3) {
                d->AddRectFilled(ImVec2(px - r, py - r * 0.6f), ImVec2(px + r, py + r * 0.6f), C(pc, a));
            } else {
                d->AddRectFilled(ImVec2(px - r, py - r), ImVec2(px + r, py + r), C(pc, a));
            }
        }
    }
}


void DrawFlowPreview(ImDrawList* d, ImVec2 center, float height, int styleIndex, float alpha, float seed) {
    DrawFlowLook(d, center, height, luma::flowstyles::kStyles[styleIndex], alpha, seed, "LUMA", -1.0f, 0.0f);
}

// YOUR STYLES: pills. Click = apply. Double click = rename (Enter keeps it,
// Esc cancels). The x on hover = delete (click it twice). Then "Update"
// (when you changed the style you were using) and "+ Save as new".
float DrawUserStyles(ImDrawList* d, ImVec2 min, ImVec2 max, float y) {
    const Palette& p = P();
    const flowcfg::State& s = flowcfg::g_state;
    {
        const float h = S(34.0f);
        float x = min.x;
        static int armed = -1;          // the pill whose x was pressed once (press again = delete)
        static double armedAt = 0.0;
        static int renaming = -1;       // the pill being renamed
        static char nameBuf[64] = "";
        static bool focusName = false;
        if (armed >= 0 && ImGui::GetTime() - armedAt > 3.0) armed = -1;
        auto& list = flowcfg::g_presets;
        if (s.user >= 0) flowcfg::g_picked = s.user;
        if (flowcfg::g_picked >= static_cast<int>(list.size())) flowcfg::g_picked = -1;
        if (renaming >= static_cast<int>(list.size())) renaming = -1;
        auto wrap = [&](float pw) {
            if (x + pw > max.x && x > min.x) {
                x = min.x;
                y += h + S(8.0f);
            }
        };
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const auto& pr = list[static_cast<std::size_t>(i)];
            const bool chosen = s.user == i;
            const bool arming = armed == i;
            const float iconW = pr.autoSaved ? S(22.0f) : 0.0f;
            if (renaming == i) {
                // the name, editable in place
                const float pw = std::max(S(200.0f), TextSize(g_fonts.semibold, nameBuf).x + S(40.0f));
                wrap(pw);
                const ImVec2 a(x, y), b(x + pw, y + h);
                d->AddRectFilled(a, b, C(p.card), h * 0.5f);
                d->AddRect(a, b, C(p.accent), h * 0.5f, 0, S(1.5f));
                ImGui::SetCursorScreenPos(ImVec2(a.x + S(14.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f - S(3.0f)));
                ImGui::PushFont(g_fonts.semibold);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, p.text);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(3.0f)));
                ImGui::SetNextItemWidth(pw - S(28.0f));
                if (focusName) {
                    ImGui::SetKeyboardFocusHere();
                    focusName = false;
                }
                const bool enter = ImGui::InputText("##rename", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue |
                                                                                               ImGuiInputTextFlags_AutoSelectAll);
                const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape);
                const bool left = ImGui::IsItemDeactivated();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                ImGui::PopFont();
                if (cancel) {
                    renaming = -1;
                } else if (enter || left) {
                    flowcfg::RenameUserStyle(i, nameBuf);
                    renaming = -1;
                }
                x += pw + S(8.0f);
                continue;
            }
            const char* label = arming ? T("lyrics.delete_confirm") : pr.name.c_str();
            const float lw = TextSize(g_fonts.semibold, label).x;
            const float pw = lw + iconW + S(58.0f);
            wrap(pw);
            const ImVec2 a(x, y), b(x + pw, y + h);
            char id[32];
            std::snprintf(id, sizeof(id), "##mystyle%d", i);
            float hover = 0.0f;
            const ImVec2 xc(b.x - S(18.0f), (a.y + b.y) * 0.5f);
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool overX = std::fabs(mouse.x - xc.x) < S(12.0f) && std::fabs(mouse.y - xc.y) < S(12.0f);
            const bool clicked = Hit(id, a, b, &hover);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !overX) {
                renaming = i;
                std::snprintf(nameBuf, sizeof(nameBuf), "%s", pr.name.c_str());
                focusName = true;
                armed = -1;
            } else if (clicked) {
                if (overX) {
                    if (arming) {
                        flowcfg::DeleteUserStyle(i);
                        armed = -1;
                        break;
                    }
                    armed = i;
                    armedAt = ImGui::GetTime();
                } else if (!arming) {
                    flowcfg::ApplyUserStyle(i);
                    flowcfg::g_picked = i;
                } else {
                    armed = -1;
                }
            }
            const ImVec4 fill = arming ? p.danger : (chosen ? p.accent : Mix(p.card, p.cardHover, hover));
            d->AddRectFilled(a, b, C(fill, arming || chosen ? 1.0f : 0.85f), h * 0.5f);
            if (!chosen && !arming) d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), flowcfg::g_picked == i ? 0.45f : 0.10f), h * 0.5f);
            const ImVec4 tc = arming || chosen ? p.onAccent : p.text;
            float tx = a.x + S(16.0f);
            if (pr.autoSaved) {
                icons::Draw(d, icons::kClock, ImVec2(tx + S(8.0f), (a.y + b.y) * 0.5f), S(15.0f), C(tc, 0.85f), S(1.6f));
                tx += iconW;
            }
            Text(d, g_fonts.semibold, ImVec2(tx, (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f), C(tc), label);
            const float xa = (hover > 0.0f || arming) ? (overX ? 1.0f : 0.55f) : 0.25f;
            icons::Draw(d, arming ? icons::kTrash : icons::kClose, xc, S(13.0f), C(tc, xa), S(1.8f));
            x += pw + S(8.0f);
        }
        // Update the style you were using (you changed it since)
        auto actionPill = [&](const char* id, const char* icon, const char* label, bool strong) {
            const float pw = TextSize(g_fonts.semibold, label).x + S(46.0f);
            wrap(pw);
            const ImVec2 a(x, y), b(x + pw, y + h);
            float hover = 0.0f;
            const bool clicked = Hit(id, a, b, &hover);
            d->AddRectFilled(a, b, C(strong ? p.accent : p.text, strong ? 0.9f + 0.1f * hover : 0.06f + 0.08f * hover), h * 0.5f);
            if (!strong) d->AddRect(a, b, C(p.accent, 0.8f), h * 0.5f, 0, S(1.5f));
            const ImVec4 tc = strong ? p.onAccent : p.text;
            icons::Draw(d, icon, ImVec2(a.x + S(18.0f), (a.y + b.y) * 0.5f), S(15.0f), C(strong ? p.onAccent : p.accent), S(2.0f));
            Text(d, g_fonts.semibold, ImVec2(a.x + S(32.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f), C(tc), label);
            x += pw + S(8.0f);
            return clicked;
        };
        if (s.found && flowcfg::g_picked >= 0 && s.user != flowcfg::g_picked) {
            char label[120];
            std::snprintf(label, sizeof(label), T("lyrics.update_style"), list[static_cast<std::size_t>(flowcfg::g_picked)].name.c_str());
            if (actionPill("##updatestyle", icons::kCheck, label, true)) flowcfg::UpdateUserStyle(flowcfg::g_picked);
        }
        if (s.found && actionPill("##savestyle", icons::kPlus, T("lyrics.save_new"), false)) flowcfg::SaveCurrentStyle();
        y += h + S(6.0f);
        Text(d, g_fonts.tiny, ImVec2(min.x + S(4.0f), y), C(p.textDim), list.empty() ? T("lyrics.no_styles") : T("lyrics.styles_hint"));
        y += g_fonts.tiny->FontSize + S(16.0f);
    }
    return y;
}

// How a style looks, from the profile's "lyricFlow" (your own or saved
// styles aren't one of the six: the stage reads their choices here).
luma::flowstyles::FlowLook LookFromJson(const nlohmann::json& j) {
    luma::flowstyles::FlowLook k = luma::flowstyles::kStyles[0];
    if (!j.is_object()) return k;
    auto num = [&](const char* key, auto& v) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_number()) v = it->get<std::decay_t<decltype(v)>>();
    };
    auto flag = [&](const char* key, bool& v) {
        const auto it = j.find(key);
        if (it != j.end() && it->is_boolean()) v = it->get<bool>();
    };
    auto color = [&](const char* key, float* c) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array() || it->size() < 3) return;
        for (int i = 0; i < 3; ++i) {
            if ((*it)[static_cast<std::size_t>(i)].is_number()) c[i] = (*it)[static_cast<std::size_t>(i)].get<float>();
        }
    };
    num("colorMode", k.colorMode);
    color("color", k.color);
    color("color2", k.color2);
    flag("outline", k.outline);
    flag("shadow", k.shadow);
    num("shadowStyle", k.shadowStyle);
    num("appear", k.appear);
    num("leave", k.leave);
    num("motion", k.motion);
    num("speed", k.speed);
    num("fill", k.fill);
    num("unsung", k.unsung);
    num("glow", k.glow);
    num("glowColor", k.glowColor);
    num("glowStrength", k.glowStrength);
    num("glowWhere", k.glowWhere);
    num("vfx", k.vfx);
    num("vfxAmount", k.vfxAmount);
    num("vfxColor", k.vfxColor);
    return k;
}

// THE STAGE of the lyrics section: a little Minecraft night (sky, stars, the
// square moon, a blocky hill) with the line being sung drawn in YOUR style,
// filling as it's sung. "In the sky" puts it high; "in the world", low, over
// the hill. The switch at the top right turns them on and off.
void DrawLyricsStage(ImDrawList* d, ImVec2 a, ImVec2 b, const media::Track& t) {
    const Palette& p = P();
    const flowcfg::State& s = flowcfg::g_state;
    const float R = S(18.0f), w = b.x - a.x, h = b.y - a.y;
    const float now = static_cast<float>(ImGui::GetTime());
    // the sky: night blue, a little of the album's color in it
    const ImVec4 top = Mix(ImVec4(0.04f, 0.05f, 0.11f, 1), p.accent, 0.10f);
    const ImVec4 low = Mix(ImVec4(0.15f, 0.19f, 0.33f, 1), p.accent, 0.16f);
    d->AddRectFilled(a, ImVec2(b.x, a.y + R * 2.0f), C(top), R, ImDrawFlags_RoundCornersTop);
    d->AddRectFilled(ImVec2(a.x, b.y - R * 2.0f), b, C(low), R, ImDrawFlags_RoundCornersBottom);
    d->AddRectFilledMultiColor(ImVec2(a.x, a.y + R), ImVec2(b.x, b.y - R), C(top), C(top), C(low), C(low));
    // stars (square, like the game's), twinkling
    for (int i = 0; i < 46; ++i) {
        const float sx = a.x + R + Hash(i, 11) * (w - R * 2.0f);
        const float sy = a.y + R * 0.6f + Hash(i, 29) * h * 0.55f;
        const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(now * (0.8f + Hash(i, 5) * 1.6f) + static_cast<float>(i)));
        const float r = Hash(i, 3) > 0.85f ? S(1.6f) : S(1.0f);
        d->AddRectFilled(ImVec2(sx - r, sy - r), ImVec2(sx + r, sy + r), C(ImVec4(1, 1, 1, 1), 0.55f * tw));
    }
    // the moon: a square, with its soft light
    {
        const ImVec2 mc(b.x - w * 0.16f, a.y + h * 0.26f);
        const float m = S(13.0f);
        SoftLight(d, mc, m * 5.0f, ImVec4(0.85f, 0.88f, 1.0f, 1), 0.22f);
        d->AddRectFilled(ImVec2(mc.x - m, mc.y - m), ImVec2(mc.x + m, mc.y + m), C(ImVec4(0.92f, 0.93f, 0.86f, 1)));
        d->AddRectFilled(ImVec2(mc.x - m * 0.5f, mc.y - m * 0.2f), ImVec2(mc.x - m * 0.1f, mc.y + m * 0.2f), C(ImVec4(0.78f, 0.79f, 0.72f, 1)));
    }
    // the hill: blocks, a ground band with round bottom corners (so nothing
    // sticks out of the stage), and steps above it away from the corners
    const float ground = b.y - h * 0.16f;
    const ImVec4 grass = Mix(ImVec4(0.07f, 0.13f, 0.08f, 1), p.accent, 0.06f);
    const ImVec4 dirt = Mix(ImVec4(0.05f, 0.06f, 0.06f, 1), p.accent, 0.04f);
    d->AddRectFilled(ImVec2(a.x, ground), b, C(dirt), R, ImDrawFlags_RoundCornersBottom);
    d->AddRectFilled(ImVec2(a.x, ground), ImVec2(b.x, ground + S(4.0f)), C(grass));
    const float blk = S(12.0f);
    float level = 0.0f;
    for (float x = a.x + R; x + blk <= b.x - R; x += blk) {
        const int col = static_cast<int>((x - a.x) / blk);
        const float wave = std::sin(static_cast<float>(col) * 0.21f) * 0.5f + 0.5f;
        const float target = std::floor(wave * 3.0f + Hash(col, 7) * 1.2f);
        level += (target - level) > 0.0f ? 1.0f : (target < level ? -1.0f : 0.0f);
        level = std::clamp(level, 0.0f, 3.0f);
        if (level <= 0.0f) continue;
        const float y0 = ground - level * blk;
        d->AddRectFilled(ImVec2(x, y0), ImVec2(x + blk, ground), C(dirt));
        d->AddRectFilled(ImVec2(x, y0), ImVec2(x + blk, y0 + S(4.0f)), C(grass));
    }
    // the words: the line being sung, filling as it's sung (else the song's
    // name, or Luma's, filling in a loop)
    const luma::flowstyles::FlowLook look = s.style >= 0 && s.style < luma::flowstyles::kCount ? luma::flowstyles::kStyles[s.style]
                                                                                                  : LookFromJson(s.look);
    std::string text = "LUMA";
    float progress = -1.0f;
    if (HasLyricLines(t)) {
        int end = 0;
        const lyrics::Line* line = LineOf(g_lyricLine, &end);
        if (line == nullptr && !g_lyrics.lines.empty()) line = &g_lyrics.lines[0];  // (before the first line: the first one, waiting)
        if (line != nullptr && line->text.find_first_not_of(" \t") != std::string::npos) {
            text = line->text;
            const int ms = LyricsMs(t);
            progress = end > line->ms ? std::clamp(static_cast<float>(ms - line->ms) / static_cast<float>(end - line->ms), 0.0f, 1.0f) : 0.0f;
        }
    } else if (t.present && t.title[0] != '\0') {
        text = t.title;
    }
    static float where = 1.0f;  // 1 = sky, 0 = world (it slides when you change it)
    where += ((s.mode == 1 ? 1.0f : 0.0f) - where) * std::min(1.0f, Dt() * 6.0f);
    const float ty = a.y + h * (0.52f - 0.22f * where);
    const float alpha = s.on ? 1.0f : 0.3f;
    d->PushClipRect(a, b, true);
    DrawFlowLook(d, ImVec2((a.x + b.x) * 0.5f, ty), h * 0.72f, look, alpha * g_fade, 0.0f, text.c_str(), progress, w - S(120.0f));
    d->PopClipRect();
    d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), 0.10f), R);
    // over it: the style's name (left) and the switch (right)
    {
        const char* name = s.user >= 0 && s.user < static_cast<int>(flowcfg::g_presets.size())
                               ? flowcfg::g_presets[static_cast<std::size_t>(s.user)].name.c_str()
                               : (s.style >= 0 && s.style < luma::flowstyles::kCount ? T(kStyleKeys[s.style][0]) : T("lyrics.my_style"));
        const ImVec2 ns = TextSize(g_fonts.tiny, name);
        const ImVec2 pa(a.x + S(14.0f), a.y + S(14.0f)), pb(pa.x + ns.x + S(22.0f), pa.y + ns.y + S(10.0f));
        d->AddRectFilled(pa, pb, C(ImVec4(0, 0, 0, 1), 0.35f), (pb.y - pa.y) * 0.5f);
        Text(d, g_fonts.tiny, ImVec2(pa.x + S(11.0f), pa.y + S(5.0f)), C(ImVec4(1, 1, 1, 1), 0.9f), name);
    }
    {
        bool on = s.on;
        const ImVec2 tp(b.x - S(64.0f), a.y + S(14.0f));
        const char* label = T("lyrics.on");
        const ImVec2 ls = TextSize(g_fonts.tiny, label);
        Text(d, g_fonts.tiny, ImVec2(tp.x - S(10.0f) - ls.x, tp.y + S(13.0f) - ls.y * 0.5f), C(ImVec4(1, 1, 1, 1), 0.85f), label);
        if (Toggle("##lyr_on", tp, &on)) flowcfg::Change([&](nlohmann::json& f) { f["on"] = on; });
    }
}

// THE SIX READY-MADE STYLES: a 3 x 2 grid of cards, each with its preview.
// ("appear" 0..1: the cards rise in, one after the other)
void DrawStyleCards(ImDrawList* d, ImVec2 min, ImVec2 max, float appear) {
    const Palette& p = P();
    const flowcfg::State& s = flowcfg::g_state;
    const float W = max.x - min.x;
    const float y = min.y;
    const float gap = S(14.0f);
    const float cw = (W - gap * 2.0f) / 3.0f;
    const float ch = std::max(S(120.0f), (max.y - y - gap) * 0.5f);
    const float dim = s.on ? 1.0f : 0.55f;  // (off: they're still chosen, just dimmer)
    for (int i = 0; i < luma::flowstyles::kCount; ++i) {
        const int cardStart = d->VtxBuffer.Size;
        struct CardIn {  // (each card rises in a moment after the one before)
            ImDrawList* d;
            int start;
            float in;
            ~CardIn() { AnimateSince(d, start, in, ImVec2(0.0f, (1.0f - in) * S(18.0f))); }
        } cardIn{d, cardStart, EaseOutCubic(std::clamp(appear * 2.2f - 0.35f - static_cast<float>(i) * 0.12f, 0.0f, 1.0f))};
        const int col = i % 3, row = i / 3;
        const ImVec2 cmin(min.x + col * (cw + gap), y + row * (ch + gap));
        const ImVec2 cmax(cmin.x + cw, cmin.y + ch);
        char id[24];
        std::snprintf(id, sizeof(id), "##flowstyle%d", i);
        float hover = 0.0f;
        const bool chosen = s.style == i;
        if (Hit(id, cmin, cmax, &hover)) flowcfg::ApplyStyle(i);
        const float lit = Anim(ImGui::GetID(id) + 3, chosen, 8.0f);
        const float lift = S(3.0f) * hover;
        const ImVec2 a(cmin.x, cmin.y - lift), b(cmax.x, cmax.y - lift);
        if (lit > 0.01f) SoftLightOval(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), cw * 0.62f, ch * 0.70f, p.accent, 0.45f * lit);
        Card(d, a, b, hover * 0.6f);
        // the preview's stage: a dark window inside the card
        const float nameH = g_fonts.semibold->FontSize + g_fonts.tiny->FontSize + S(26.0f);
        const ImVec2 vmin(a.x + S(10.0f), a.y + S(10.0f)), vmax(b.x - S(10.0f), b.y - nameH);
        d->AddRectFilled(vmin, vmax, C(ImVec4(0.03f, 0.03f, 0.05f, 1.0f), 0.55f), S(kRound) - S(4.0f));
        d->PushClipRect(vmin, vmax, true);
        DrawFlowPreview(d, ImVec2((vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f), vmax.y - vmin.y, i, g_fade * dim,
                        static_cast<float>(i) * 0.37f);
        d->PopClipRect();
        // its name and what it does
        const float tx = a.x + S(14.0f);
        float ty = vmax.y + S(9.0f);
        Text(d, g_fonts.semibold, ImVec2(tx, ty), C(p.text), T(kStyleKeys[i][0]));
        if (chosen) {
            const ImVec2 c(b.x - S(22.0f), ty + g_fonts.semibold->FontSize * 0.5f);
            d->AddCircleFilled(c, S(9.0f), C(p.accent), 24);
            icons::Draw(d, kIconCheck, c, S(11.0f), C(p.onAccent), S(2.2f));
        }
        ty += g_fonts.semibold->FontSize + S(3.0f);
        TextFit(d, g_fonts.tiny, ImVec2(tx, ty), C(p.textDim), T(kStyleKeys[i][1]), b.x - tx - S(10.0f));
        // the chosen one: the accent around it
        if (lit > 0.01f) d->AddRect(a, b, C(p.accent, lit), S(kRound), 0, S(2.0f));
    }
}



// The album art (or a placeholder) at "center", "size" wide, with its old
// one fading out.
void DrawArtAt(ImDrawList* d, ImVec2 center, float size, float alpha, float rounding = -1.0f) {
    const Palette& p = P();
    if (rounding < 0.0f) rounding = S(22.0f);
    const float pop = 0.94f + 0.06f * EaseOutCubic(g_artFade);
    auto one = [&](ID3D11ShaderResourceView* view, float a, float scale) {
        const float half = size * 0.5f * scale;
        const ImVec2 mn(center.x - half, center.y - half), mx(center.x + half, center.y + half);
        if (view == nullptr) {
            d->AddRectFilled(mn, mx, C(p.accent, a * alpha), rounding);
            icons::Draw(d, icons::kMusic, center, size * 0.26f, C(p.onAccent, a * alpha * 0.7f), S(1.6f));
            return;
        }
        DrawArtImage(d, view, mn, mx, C(ImVec4(1, 1, 1, 1), a * alpha), rounding);
    };
    if (g_artOld.view != nullptr && g_artFade < 1.0f) one(g_artOld.view, 1.0f, 1.0f);
    one(g_art.view, g_artOld.view != nullptr ? g_artFade : 1.0f, pop);
    const float half = size * 0.5f;
    d->AddRect(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), C(ImVec4(1, 1, 1, 1), 0.08f * alpha),
               rounding);
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

    // (the heart, in the art's corner: your favorites)
    DrawArtHeart(d, artC, artSize, t, alpha);

    // --- The words, the lyrics, the progress, the controls ---------------------------------
    const float x = x0 + artSize + discReach * g_discOut + S(54.0f);
    const float w = infoW;
    const float slide = (1.0f - EaseOutCubic(g_trackIn)) * S(18.0f);
    const float ta = g_trackIn * alpha;
    const float lyricsH = CompactLyricsHeight();
    float y = artC.y - S(108.0f) - lyricsH * 0.5f;
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
            // (Luma's own Spotify player: "Spotify · Luma", not "Edge")
            const char* app = IsSpotifyApp(t.app) && std::strstr(t.app, "Spotify") == nullptr ? "Spotify  ·  Luma" : t.app;
            const float ax = lx + S(32.0f);
            const ImVec2 as = TextSize(g_fonts.tiny, app);
            d->AddRectFilled(ImVec2(ax, y - S(3.0f)), ImVec2(ax + as.x + S(18.0f), y + as.y + S(3.0f)), C(ImVec4(1, 1, 1, 1), 0.10f * alpha),
                             S(12.0f));
            Text(d, g_fonts.tiny, ImVec2(ax + S(9.0f), y), C(p.text, 0.8f * alpha), app);
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
    y += g_fonts.semibold->FontSize + S(16.0f);
    // the lyrics: the line being sung and the next one
    DrawCompactLyrics(d, x, y, w, t, alpha);
    y += lyricsH + S(10.0f);
    SeekBar(d, "##seek", x, x + w, y, t, alpha, true);
    y += S(58.0f);
    const float savedFade = g_fade;
    g_fade *= alpha;
    // shuffle, previous, play, next, repeat
    spotify::Player sp;
    const bool viaSpotify = SpotifyDrivesHome(t, sp);  // (Windows doesn't pass these from Spotify: its Web API does)
    {
        const bool on = viaSpotify ? sp.shuffle : t.shuffle;
        if (IconToggle(d, "##shuffle", ImVec2(x + S(14.0f), y), icons::kShuffle, S(18.0f), on, viaSpotify || t.canShuffle)) {
            if (viaSpotify) {
                spotify::PlayerShuffle(!on);
            } else {
                media::SetShuffle(!on);
            }
        }
    }
    if (RoundButton(d, "##prev", ImVec2(x + S(66.0f), y), S(24.0f), icons::kSkipBackFilled, S(18.0f), false, t.canPrevious)) media::Previous();
    {
        const ImVec2 c(x + S(138.0f), y);
        if (t.playing) d->AddCircleFilled(c, S(32.0f) + S(12.0f) * g_bassSmooth, C(p.accent, 0.20f + 0.25f * g_bassSmooth), 48);
        if (RoundButton(d, "##playpause", c, S(32.0f), g_playMorph > 0.5f ? icons::kPlayFilled : icons::kPauseFilled, S(22.0f), true,
                        t.canPlayPause)) {
            media::PlayPause();
        }
    }
    if (RoundButton(d, "##next", ImVec2(x + S(210.0f), y), S(24.0f), icons::kSkipForwardFilled, S(18.0f), false, t.canNext)) media::Next();
    {
        // off -> the whole list -> this song -> off
        const int mode = viaSpotify ? sp.repeat : t.repeat;
        const int nextMode = mode == 0 ? 2 : (mode == 2 ? 1 : 0);
        if (IconToggle(d, "##repeat", ImVec2(x + S(262.0f), y), mode == 1 ? icons::kRepeat1 : icons::kRepeat, S(18.0f), mode != 0,
                       viaSpotify || t.canRepeat)) {
            if (viaSpotify) {
                spotify::PlayerRepeat(nextMode);
            } else {
                media::SetRepeat(nextMode);
            }
        }
    }
    // at the right: the volume, the lyrics view, the options
    {
        const ImVec2 opt(x + w - S(14.0f), y);
        if (IconToggle(d, "##options", opt, icons::kEllipsis, S(18.0f), g_optionsOpen)) {
            g_optionsOpen = !g_optionsOpen;
            g_optionsAnchor = opt;
        }
        const bool canView = g_lyricsShow > 0.5f;
        if (IconToggle(d, "##lyricsview", ImVec2(x + w - S(54.0f), y), icons::kMicVocal, S(18.0f), g_lyricsView, canView)) {
            g_lyricsView = !g_lyricsView;
        }
        if (w >= S(480.0f)) {
            if (viaSpotify) {
                SpotifyVolumeBar(d, ImVec2(x + w - S(190.0f), y), S(86.0f), sp, alpha);  // (Spotify's device volume)
            } else {
                VolumeBar(d, ImVec2(x + w - S(190.0f), y), S(86.0f), t, alpha);
            }
        }
    }
    g_fade = savedFade;
}

// =============================================================================
//  THE SPOTIFY SECTION (part S1: the connection)
// =============================================================================
//  The same connection as the game (client/core/spotify.cpp): your own small
//  Spotify app (its Client ID), Spotify's own sign-in page in your browser,
//  and the pass saved encrypted in %APPDATA%\Luma\spotify.dat. Connect here
//  or in the game: both are connected. The Client ID is kept in your Luma
//  profile (the same setting the game's Music page uses).
// =============================================================================

// Spotify's logo (its official shape: the circle and its three waves), from
// Simple Icons (simpleicons.org, CC0). Spotify's own brand rules: use it as
// it is, in Spotify green (or white/black), never changed.
constexpr const char* kSpotifyLogo =
    "F M12 0C5.4 0 0 5.4 0 12s5.4 12 12 12 12-5.4 12-12S18.66 0 12 0zm5.521 17.34c-.24.359-.66.48-1.021.24-2.82-1.74-6.36-2.101-"
    "10.561-1.141-.418.122-.779-.179-.899-.539-.12-.421.18-.78.54-.9 4.56-1.021 8.52-.6 11.64 1.32.42.18.479.659.301 1.02zm1.44-"
    "3.3c-.301.42-.841.6-1.262.3-3.239-1.98-8.159-2.58-11.939-1.38-.479.12-1.02-.12-1.14-.6-.12-.48.12-1.021.6-1.141C9.6 9.9 15 "
    "10.561 18.72 12.84c.361.181.54.78.241 1.2zm.12-3.36C15.24 8.4 8.82 8.16 5.16 9.301c-.6.179-1.2-.181-1.38-.721-.18-.601.18-1.2"
    ".72-1.381 4.26-1.26 11.28-1.02 15.721 1.621.539.3.719 1.02.419 1.56-.299.421-1.02.599-1.559.3z";
const ImVec4 kSpotifyGreen(0.118f, 0.843f, 0.376f, 1.0f);  // #1ED760

void DrawSpotifyLogo(ImDrawList* d, ImVec2 c, float size, float alpha) {
    d->AddCircleFilled(c, size * 0.46f, C(ImVec4(0, 0, 0, 1), alpha), 48);  // (the waves are holes: black under them)
    icons::Draw(d, kSpotifyLogo, c, size, C(kSpotifyGreen, alpha));
}

// A Client ID looks like 32 letters/numbers from 0-9 and a-f.
bool LooksLikeClientId(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string CleanClientId(const char* text) {
    std::string out;
    for (const char* c = text; *c != '\0'; ++c) {
        if (!std::isspace(static_cast<unsigned char>(*c)) && *c != '"' && *c != '\'') out.push_back(*c);
    }
    return out;
}

// Keeps the connection's Client ID the one in your profile (the game's too).
void SyncSpotifyClientId() {
    static std::string applied = "\x01";
    if (flowcfg::g_state.spotifyClientId != applied) {
        applied = flowcfg::g_state.spotifyClientId;
        spotify::SetClientId(applied);
    }
}

void SaveSpotifyClientId(const std::string& id) {
    flowcfg::ChangeProfile([&](nlohmann::json& profile) { flowcfg::SettingsOf(profile)["spotifyClientId"] = id; });
    SyncSpotifyClientId();
}

// A small button made of text in a pill (Open, Copy, Paste...).
bool SmallPill(ImDrawList* d, const char* id, ImVec2 pos, const char* icon, const char* label, bool strong, float* outWidth = nullptr) {
    const Palette& p = P();
    const ImVec2 ts = TextSize(g_fonts.semibold, label);
    const float w = ts.x + (icon != nullptr ? S(44.0f) : S(28.0f)), h = S(34.0f);
    if (outWidth != nullptr) *outWidth = w;
    float hover = 0.0f;
    const bool clicked = Hit(id, pos, ImVec2(pos.x + w, pos.y + h), &hover);
    const float press = Anim(ImGui::GetID(id) + 2, ImGui::IsItemActive(), 22.0f);
    const ImVec2 a(pos.x + S(1.0f) * press, pos.y + S(1.0f) * press), b(pos.x + w - S(1.0f) * press, pos.y + h - S(1.0f) * press);
    if (strong) {
        d->AddRectFilled(a, b, C(Mix(kSpotifyGreen, ImVec4(1, 1, 1, 1), 0.12f * hover)), h * 0.5f);
    } else {
        d->AddRectFilled(a, b, C(p.text, 0.07f + 0.07f * hover), h * 0.5f);
        d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), 0.12f), h * 0.5f);
    }
    const ImVec4 tc = strong ? ImVec4(0.02f, 0.02f, 0.02f, 1.0f) : p.text;
    float tx = a.x + S(14.0f);
    if (icon != nullptr) {
        icons::Draw(d, icon, ImVec2(tx + S(8.0f), (a.y + b.y) * 0.5f), S(15.0f), C(tc), S(1.8f));
        tx += S(18.0f);
    }
    Text(d, g_fonts.semibold, ImVec2(tx, (a.y + b.y - ts.y) * 0.5f), C(tc), label);
    return clicked;
}

// --- THE PLAYER (part S2) ---------------------------------------------------------------------
//  What plays in your Spotify, on any device, controlled by Spotify's Web API:
//  the art, the song, the progress, shuffle / previous / play / next / repeat,
//  the device's volume, the device it plays on (and the others), what comes
//  next, and the heart = your Liked songs on Spotify.

// Is Spotify connected? (then the player can be used)
bool SpotifyConnected() {
    return spotify::GetStatus().state == spotify::State::Connected;
}

// Is the song on Home the one Spotify plays? Then Home's shuffle, repeat and
// heart use Spotify (Windows doesn't pass them from Spotify).
bool SpotifyDrivesHome(const media::Track& t, spotify::Player& out) {
    if (!t.present || !IsSpotifyApp(t.app) || !SpotifyConnected()) return false;
    out = spotify::GetPlayer();
    return out.valid && out.hasDevice && !out.title.empty() && out.title == t.title;
}

std::string g_spotifyNotice;  // (shown under the controls for a while)

const char* SpotifyErrorText(const std::string& e) {
    if (e == "premium") return T("spotify.err_premium");
    if (e == "no_device") return T("spotify.err_no_device");
    if (e == "busy") return T("spotify.err_busy");
    return nullptr;
}

// The list of your devices (a small glass menu above "anchor").
bool g_devicesOpen = false;
float g_devicesAnim = 0.0f;
ImVec2 g_devicesAnchor;
std::vector<spotify::Device> g_deviceList;
std::uint32_t g_deviceListVersion = 0;

const char* DeviceIcon(const std::string& type) {
    if (type == "Computer") return icons::kAppWindow;
    if (type == "Smartphone" || type == "Tablet") return icons::kSquare;
    if (type == "Speaker" || type == "AVR" || type == "STB" || type == "TV") return icons::kVolume;
    return icons::kMusic2;
}

void DrawDevicesMenu(ImDrawList* d) {
    g_devicesAnim += ((g_devicesOpen ? 1.0f : 0.0f) - g_devicesAnim) * std::min(1.0f, Dt() * 16.0f);
    if (g_devicesAnim < 0.01f) return;
    std::uint32_t version = g_deviceListVersion;
    if (spotify::Devices(g_deviceListVersion, g_deviceList, version)) g_deviceListVersion = version;
    const Palette& p = P();
    const int start = d->VtxBuffer.Size;
    const float itemH = S(44.0f), width = S(300.0f);
    const int rows = std::max(1, static_cast<int>(g_deviceList.size()));
    const float height = S(40.0f) + itemH * static_cast<float>(rows) + S(10.0f);
    const ImVec2 max(g_devicesAnchor.x + width * 0.5f, g_devicesAnchor.y - S(22.0f));
    const ImVec2 min(max.x - width, max.y - height);
    d->AddRectFilled(ImVec2(min.x, min.y + S(6.0f)), ImVec2(max.x, max.y + S(8.0f)), Col(ImVec4(0, 0, 0, 1), 0.25f), S(14.0f));
    d->AddRectFilled(min, max, Col(Mix(p.card, ImVec4(1, 1, 1, 1), 0.04f), 0.97f), S(14.0f));
    d->AddRect(min, max, Col(ImVec4(1, 1, 1, 1), 0.12f), S(14.0f));
    Text(d, g_fonts.semibold, ImVec2(min.x + S(16.0f), min.y + S(12.0f)), Col(p.text), T("spotify.devices"));
    float y = min.y + S(40.0f);
    bool done = false;
    if (g_deviceList.empty()) {
        Text(d, g_fonts.tiny, ImVec2(min.x + S(16.0f), y + S(12.0f)), Col(p.textDim), T("spotify.no_devices"));
    }
    for (std::size_t i = 0; i < g_deviceList.size(); ++i) {
        const spotify::Device& dv = g_deviceList[i];
        const ImVec2 imin(min.x + S(6.0f), y), imax(max.x - S(6.0f), y + itemH);
        char id[32];
        std::snprintf(id, sizeof(id), "##dev%zu", i);
        float hover = 0.0f;
        if (Hit(id, imin, imax, &hover) && !dv.active) {
            spotify::PlayerTransfer(dv.id);
            done = true;
        }
        d->AddRectFilled(imin, imax, Col(p.text, 0.07f * hover), S(9.0f));
        const ImVec4 col = dv.active ? kSpotifyGreen : p.text;
        icons::Draw(d, DeviceIcon(dv.type), ImVec2(imin.x + S(20.0f), (imin.y + imax.y) * 0.5f), S(17.0f), Col(col), S(1.6f));
        TextFit(d, g_fonts.semibold, ImVec2(imin.x + S(42.0f), imin.y + S(5.0f)), Col(col), dv.name.c_str(), imax.x - imin.x - S(80.0f));
        Text(d, g_fonts.tiny, ImVec2(imin.x + S(42.0f), imin.y + S(5.0f) + g_fonts.semibold->FontSize), Col(p.textDim),
             dv.active ? T("spotify.playing_here") : dv.type.c_str());
        if (dv.active) icons::Draw(d, kIconCheck, ImVec2(imax.x - S(18.0f), (imin.y + imax.y) * 0.5f), S(14.0f), Col(kSpotifyGreen), S(2.2f));
        y += itemH;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool inside = mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y;
    if (g_devicesOpen && (done || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inside && !ImGui::IsAnyItemHovered()) ||
                          ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        g_devicesOpen = false;
    }
    const float e = EaseOutCubic(g_devicesAnim);
    AnimateSince(d, start, e, ImVec2(0.0f, (1.0f - e) * S(10.0f)));
}

// The device's volume (Spotify's), like Home's bar.
void SpotifyVolumeBar(ImDrawList* d, ImVec2 left, float width, const spotify::Player& pl, float alpha) {
    const Palette& p = P();
    {
        namespace eng = luma::launcher::spotifyengine;
        const bool luma = eng::GetState() == eng::State::Ready && pl.device.id == eng::DeviceId();
        if (!luma && (pl.device.volume < 0 || !pl.device.canVolume)) return;
    }
    icons::Draw(d, icons::kVolume, left, S(17.0f), C(p.text, 0.75f * alpha), S(1.7f));
    const float x0 = left.x + S(18.0f), x1 = x0 + width;
    float hover = 0.0f;
    Hit("##sp_volume", ImVec2(x0 - S(6.0f), left.y - S(10.0f)), ImVec2(x1 + S(6.0f), left.y + S(10.0f)), &hover);
    static float shown = -1.0f;
    static int sent = -1;
    const float press = Anim(ImGui::GetID("##sp_volume") + 4, ImGui::IsItemActive(), 16.0f);
    namespace eng = luma::launcher::spotifyengine;
    const bool luma = eng::GetState() == eng::State::Ready && pl.device.id == eng::DeviceId();
    const float real = luma ? static_cast<float>(eng::Volume()) / 100.0f : static_cast<float>(pl.device.volume) / 100.0f;
    if (ImGui::IsItemActive()) {
        shown = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, width), 0.0f, 1.0f);
        const int v = static_cast<int>(std::lround(shown * 100.0f));
        if (v != sent) {
            if (luma) {
                eng::SetVolume(v);  // (Luma's own player: Windows' volume of its sound, at once)
            } else {
                spotify::PlayerVolume(v);
            }
            sent = v;
        }
    } else {
        sent = -1;
        shown = shown < 0.0f ? real : shown + (real - shown) * std::min(1.0f, Dt() * 12.0f);
    }
    hover = std::max(hover, press);
    const float th = S(4.0f) + S(2.0f) * hover;
    d->AddRectFilled(ImVec2(x0, left.y - th * 0.5f), ImVec2(x1, left.y + th * 0.5f), C(ImVec4(1, 1, 1, 1), 0.16f * alpha), th);
    d->AddRectFilled(ImVec2(x0, left.y - th * 0.5f), ImVec2(x0 + width * shown, left.y + th * 0.5f), C(p.text, 0.85f * alpha), th);
    if (hover > 0.0f) d->AddCircleFilled(ImVec2(x0 + width * shown, left.y), S(5.0f) * hover, C(p.text, alpha), 16);
}

void SpotifySeekTo(double seconds) {
    spotify::PlayerSeek(seconds);
}

// "Up next": the queue (the same list the game shows).
std::vector<spotify::QueueItem> g_spQueue;
std::uint32_t g_spQueueVersion = 0;

void DrawSpotifyPlayer(ImDrawList* d, ImVec2 min, ImVec2 max, const spotify::Status& st) {
    const Palette& p = P();
    const spotify::Player pl = spotify::GetPlayer();
    const float W = max.x - min.x, H = max.y - min.y;
    const float now = static_cast<float>(ImGui::GetTime());
    {
        std::uint32_t version = g_spQueueVersion;
        if (spotify::Queue(g_spQueueVersion, g_spQueue, version)) g_spQueueVersion = version;
    }
    // Nothing playing anywhere: say so, and offer your devices.
    if (pl.valid && !pl.hasDevice) {
        const ImVec2 c(min.x + W * 0.5f, min.y + H * 0.36f);
        DrawSpotifyLogo(d, c, S(64.0f), g_fade * (0.75f + 0.25f * std::sin(now * 1.6f)));
        const char* title = T("spotify.nothing_title");
        const ImVec2 ts = TextSize(g_fonts.title, title);
        Text(d, g_fonts.title, ImVec2(c.x - ts.x * 0.5f, c.y + S(52.0f)), C(p.text), title);
        const char* text = T("spotify.nothing_text");
        const float tw = std::min(W * 0.6f, S(520.0f));
        const ImVec2 xs = g_fonts.regular->CalcTextSizeA(g_fonts.regular->FontSize, FLT_MAX, tw, text);
        TextWrap(d, g_fonts.regular, ImVec2(c.x - std::min(xs.x, tw) * 0.5f, c.y + S(60.0f) + ts.y), C(p.textDim), text, tw);
        float bw = 0.0f;
        const char* label = T("spotify.choose_device");
        const float lw = TextSize(g_fonts.semibold, label).x + S(44.0f);
        const ImVec2 bp(c.x - lw * 0.5f, c.y + S(76.0f) + ts.y + xs.y);
        if (SmallPill(d, "##sp_choose", bp, icons::kAppWindow, label, true, &bw)) {
            spotify::RequestDevices();
            g_devicesOpen = !g_devicesOpen;
            g_devicesAnchor = ImVec2(bp.x + bw * 0.5f, bp.y);
        }
        return;
    }
    if (!pl.valid) {
        Spinner(d, ImVec2(min.x + W * 0.5f, min.y + H * 0.4f), S(16.0f), C(kSpotifyGreen));
        return;
    }
    // --- Left: the art, the song, the progress, the controls -------------------------------------
    const float artSize = std::clamp(std::min(H - S(230.0f), W * 0.32f), S(180.0f), S(330.0f));
    const float colW = std::max(artSize, S(360.0f));
    const float lx = min.x + S(8.0f);
    const ImVec2 artMin(lx + (colW - artSize) * 0.5f, min.y), artMax(artMin.x + artSize, min.y + artSize);
    const ImVec2 artC((artMin.x + artMax.x) * 0.5f, (artMin.y + artMax.y) * 0.5f);
    SoftLight(d, artC, artSize * 0.95f, p.accent, (0.55f + 0.35f * g_bassSmooth) * g_fade);
    SoftLightOval(d, ImVec2(artC.x, artC.y + artSize * 0.08f), artSize * 0.70f, artSize * 0.62f, ImVec4(0, 0, 0, 1), 0.9f * g_fade);
    {
        const textures::Texture* tex = pl.artPath.empty() ? nullptr : textures::GetAsync(pl.artPath);
        if (tex != nullptr) {
            d->AddImageRounded(tex->id, artMin, artMax, ImVec2(0, 0), ImVec2(1, 1), C(ImVec4(1, 1, 1, 1)), S(16.0f));
        } else {
            d->AddRectFilled(artMin, artMax, C(p.card), S(16.0f));
            DrawSpotifyLogo(d, artC, artSize * 0.3f, 0.5f * g_fade);
        }
        d->AddRect(artMin, artMax, C(ImVec4(1, 1, 1, 1), 0.08f), S(16.0f));
    }
    // the heart: your Liked songs, on Spotify
    {
        const float r = S(19.0f);
        const ImVec2 c(artMax.x - r - S(10.0f), artMax.y - r - S(10.0f));
        float hover = 0.0f;
        static float pop = 0.0f;
        if (Hit("##sp_heart", ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), &hover, pl.likedKnown && !pl.trackId.empty())) {
            spotify::PlayerLike(!pl.liked);
            pop = 1.0f;
        }
        pop = std::max(0.0f, pop - Dt() * 4.0f);
        const float grow = 1.0f + 0.25f * std::sin(pop * 3.14159f);
        d->AddCircleFilled(c, r * (1.0f + 0.06f * hover), C(ImVec4(0.05f, 0.05f, 0.06f, 1.0f), 0.55f + 0.2f * hover), 32);
        icons::Draw(d, pl.liked ? icons::kHeartFilled : icons::kHeart, c, std::round(S(18.0f) * grow),
                    C(pl.liked ? HeartColor() : ImVec4(1, 1, 1, 1), pl.likedKnown ? 1.0f : 0.4f), S(1.8f));
    }
    float y = artMax.y + S(18.0f);
    TextFit(d, g_fonts.title, ImVec2(lx, y), C(p.text), pl.title.c_str(), colW);
    y += g_fonts.title->FontSize + S(2.0f);
    {
        const std::string line = pl.album.empty() ? pl.artist : pl.artist + "  ·  " + pl.album;
        TextFit(d, g_fonts.regular, ImVec2(lx, y), C(p.textDim), line.c_str(), colW);
    }
    y += g_fonts.regular->FontSize + S(22.0f);
    // the progress (Spotify's)
    {
        media::Track fake;
        fake.present = true;
        fake.position = spotify::PositionSeconds(pl);
        fake.duration = pl.durationMs / 1000.0;
        fake.canSeek = true;
        SeekBar(d, "##sp_seek", lx, lx + colW, y, fake, 1.0f, true, SpotifySeekTo);
    }
    y += S(58.0f);
    // shuffle, previous, play, next, repeat
    {
        const float cx = lx + colW * 0.5f;
        if (IconToggle(d, "##sp_shuffle", ImVec2(cx - S(124.0f), y), icons::kShuffle, S(18.0f), pl.shuffle)) spotify::PlayerShuffle(!pl.shuffle);
        if (RoundButton(d, "##sp_prev", ImVec2(cx - S(72.0f), y), S(24.0f), icons::kSkipBackFilled, S(18.0f), false)) spotify::PlayerPrevious();
        {
            const ImVec2 c(cx, y);
            if (pl.playing) d->AddCircleFilled(c, S(32.0f) + S(10.0f) * g_bassSmooth, C(kSpotifyGreen, 0.18f + 0.2f * g_bassSmooth), 48);
            if (RoundButton(d, "##sp_play", c, S(32.0f), pl.playing ? icons::kPauseFilled : icons::kPlayFilled, S(22.0f), true)) {
                spotify::PlayerPlay(!pl.playing);
            }
        }
        if (RoundButton(d, "##sp_next", ImVec2(cx + S(72.0f), y), S(24.0f), icons::kSkipForwardFilled, S(18.0f), false)) spotify::PlayerNext();
        {
            const int nextMode = pl.repeat == 0 ? 2 : (pl.repeat == 2 ? 1 : 0);  // off -> the list -> this song -> off
            if (IconToggle(d, "##sp_repeat", ImVec2(cx + S(124.0f), y), pl.repeat == 1 ? icons::kRepeat1 : icons::kRepeat, S(18.0f), pl.repeat != 0)) {
                spotify::PlayerRepeat(nextMode);
            }
        }
    }
    y += S(52.0f);
    // the volume and the device
    {
        SpotifyVolumeBar(d, ImVec2(lx + S(10.0f), y), S(130.0f), pl, 1.0f);
        const std::string dev = pl.device.name.empty() ? std::string(T("spotify.devices")) : pl.device.name;
        const ImVec2 ds = TextSize(g_fonts.tiny, dev.c_str());
        const float dw = std::min(ds.x + S(46.0f), colW - S(180.0f));
        const ImVec2 a(lx + colW - dw, y - S(15.0f)), b(lx + colW, y + S(15.0f));
        float hover = 0.0f;
        if (Hit("##sp_device", a, b, &hover)) {
            spotify::RequestDevices();
            g_devicesOpen = !g_devicesOpen;
            g_devicesAnchor = ImVec2((a.x + b.x) * 0.5f, a.y);
        }
        d->AddRectFilled(a, b, C(kSpotifyGreen, 0.14f + 0.10f * hover), S(15.0f));
        icons::Draw(d, DeviceIcon(pl.device.type), ImVec2(a.x + S(16.0f), y), S(14.0f), C(kSpotifyGreen), S(1.7f));
        TextFit(d, g_fonts.tiny, ImVec2(a.x + S(30.0f), y - ds.y * 0.5f), C(kSpotifyGreen), dev.c_str(), dw - S(40.0f));
    }
    y += S(26.0f);
    // why a button didn't work
    if (const char* err = SpotifyErrorText(spotify::PlayerError())) {
        TextWrap(d, g_fonts.tiny, ImVec2(lx, y), C(p.warning), err, colW);
    }

    // --- Right: up next ------------------------------------------------------------------------------
    const float rx = lx + colW + S(48.0f);
    const float rw = max.x - rx;
    if (rw < S(220.0f)) return;
    Text(d, g_fonts.semibold, ImVec2(rx, min.y), C(p.text), T("spotify.up_next"));
    float qy = min.y + g_fonts.semibold->FontSize + S(14.0f);
    if (g_spQueue.empty()) {
        Text(d, g_fonts.tiny, ImVec2(rx, qy), C(p.textDim), T("spotify.queue_empty"));
    }
    const float rowH = S(50.0f);
    for (std::size_t i = 0; i < g_spQueue.size() && qy + rowH <= max.y; ++i) {
        const auto& q = g_spQueue[i];
        const int rowStart = d->VtxBuffer.Size;
        const ImVec2 a(rx, qy), b(rx + rw, qy + rowH - S(6.0f));
        d->AddRectFilled(a, b, C(p.text, i == 0 ? 0.06f : 0.03f), S(10.0f));
        char num[8];
        std::snprintf(num, sizeof(num), "%zu", i + 1);
        const ImVec2 ns = TextSize(g_fonts.semibold, num);
        Text(d, g_fonts.semibold, ImVec2(a.x + S(20.0f) - ns.x * 0.5f, (a.y + b.y - ns.y) * 0.5f), C(p.textDim), num);
        TextFit(d, g_fonts.semibold, ImVec2(a.x + S(40.0f), a.y + S(5.0f)), C(p.text), q.title.c_str(), rw - S(110.0f));
        TextFit(d, g_fonts.tiny, ImVec2(a.x + S(40.0f), a.y + S(7.0f) + g_fonts.semibold->FontSize), C(p.textDim), q.artist.c_str(), rw - S(110.0f));
        if (q.seconds > 0) {
            char len[16];
            FormatTime(q.seconds, len, sizeof(len));
            const ImVec2 ls = TextSize(g_fonts.tiny, len);
            Text(d, g_fonts.tiny, ImVec2(b.x - ls.x - S(12.0f), (a.y + b.y - ls.y) * 0.5f), C(p.textDim), len);
        }
        // (each row comes in a moment after the one above)
        const float in = EaseOutCubic(std::clamp(g_sheet * 2.2f - 0.4f - static_cast<float>(i) * 0.08f, 0.0f, 1.0f));
        AnimateSince(d, rowStart, in, ImVec2(S(16.0f) * (1.0f - in), 0.0f));
        qy += rowH;
    }
    (void)st;
}

void Flash(const char* icon, const char* text);  // (with the keys, below)

// =============================================================================
//  SPOTIFY, THE WHOLE SECTION (part S3: C1 + C3)
// =============================================================================
//  A full section (not a window): over the album's living background.
//    - Top: what plays (art, song, controls, heart), Search, and the buttons
//      of the drawer.
//    - Tabs: Home (rows: played lately, your playlists, your favorite
//      artists and songs, saved albums) and Library (everything of yours).
//    - Pages: a playlist, an album, an artist, your Liked songs, "see all";
//      the arrow goes back.
//    - The drawer (right): the queue, your devices, the lyrics.
//    - Search: a layer over everything (Ctrl+K), with what you type.
//    - Options (right click or "..."): play, add to the queue, Luma's radio,
//      go to the artist / album, add to a playlist, Liked songs, copy the
//      link, open in Spotify.
//  Everything comes from Spotify's Web API through core/spotify.cpp (in the
//  background: the screen never waits).
// =============================================================================

namespace sp {

enum Type { kTrack, kAlbum, kArtist, kPlaylist, kShow, kEpisode, kLiked };

struct Item {
    int type = kTrack;
    std::string id, uri, name, sub, image;
    int ms = 0;
    std::vector<std::pair<std::string, std::string>> artists;  // (id, name)
    std::string albumId, albumName;
    std::string ownerId;
    int total = 0;  // songs in a playlist / album
};

// --- Reading Spotify's answers ------------------------------------------------------------------

std::string Str(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The picture closest to "want" pixels.
std::string PickImage(const nlohmann::json& j, int want = 300) {
    const auto images = j.find("images");
    if (images == j.end() || !images->is_array() || images->empty()) return {};
    std::string best;
    int bestDiff = 1 << 30;
    for (const auto& im : *images) {
        if (!im.is_object()) continue;
        const std::string url = Str(im, "url");
        const int w = im.contains("width") && im["width"].is_number() ? im["width"].get<int>() : want;
        const int diff = w >= want ? w - want : (want - w) * 4;  // (bigger is fine, smaller looks blurry)
        if (!url.empty() && diff < bestDiff) {
            best = url;
            bestDiff = diff;
        }
    }
    return best;
}

std::string JoinArtists(const Item& it) {
    std::string out;
    for (const auto& a : it.artists) {
        if (!out.empty()) out += ", ";
        out += a.second;
    }
    return out;
}

void ReadArtists(const nlohmann::json& j, Item& it) {
    const auto a = j.find("artists");
    if (a == j.end() || !a->is_array()) return;
    for (const auto& x : *a) {
        if (x.is_object()) it.artists.emplace_back(Str(x, "id"), Str(x, "name"));
    }
}

bool Parse(const nlohmann::json& j, Item& it) {
    if (!j.is_object()) return false;
    const std::string type = Str(j, "type");
    it.id = Str(j, "id");
    it.uri = Str(j, "uri");
    it.name = Str(j, "name");
    render::fontfallback::Need(it.name.c_str());  // (Japanese, Korean... names: their letters are added)
    if (type == "track") {
        it.type = kTrack;
        it.ms = j.contains("duration_ms") && j["duration_ms"].is_number() ? j["duration_ms"].get<int>() : 0;
        ReadArtists(j, it);
        it.sub = JoinArtists(it);
        render::fontfallback::Need(it.sub.c_str());
        if (j.contains("album") && j["album"].is_object()) {
            it.albumId = Str(j["album"], "id");
            it.albumName = Str(j["album"], "name");
            it.image = PickImage(j["album"], 300);  // (tiles are big: the 64 px one looked blurry)
        }
    } else if (type == "album") {
        it.type = kAlbum;
        ReadArtists(j, it);
        it.sub = JoinArtists(it);
        it.image = PickImage(j);
        it.total = j.value("total_tracks", 0);
    } else if (type == "artist") {
        it.type = kArtist;
        it.image = PickImage(j);
        it.sub = T("sp.artist");
    } else if (type == "playlist") {
        it.type = kPlaylist;
        it.image = PickImage(j);
        if (j.contains("owner") && j["owner"].is_object()) {
            it.ownerId = Str(j["owner"], "id");
            it.sub = Str(j["owner"], "display_name");
        }
        render::fontfallback::Need(it.sub.c_str());
        const auto tr = j.find("tracks");
        if (tr != j.end() && tr->is_object()) it.total = tr->value("total", 0);
        const auto items = j.find("items");  // (newer answers call it "items")
        if (it.total == 0 && items != j.end() && items->is_object()) it.total = items->value("total", 0);
    } else if (type == "show") {
        it.type = kShow;
        it.image = PickImage(j);
        it.sub = Str(j, "publisher");
    } else if (type == "episode") {
        it.type = kEpisode;
        it.image = PickImage(j, 300);
        it.ms = j.value("duration_ms", 0);
        it.sub = Str(j, "release_date");
    } else {
        return false;
    }
    return !it.uri.empty();
}

// A list of items from an answer: "items" (or "artists.items"...), each one
// the item itself or {"track": item} / {"album": item} / {"show": item}.
std::vector<Item> ParseList(const nlohmann::json& j, const char* key = "items") {
    std::vector<Item> out;
    const nlohmann::json* list = nullptr;
    if (j.is_object() && j.contains(key) && j[key].is_array()) {
        list = &j[key];
    } else if (j.is_object() && j.contains(key) && j[key].is_object() && j[key].contains("items") && j[key]["items"].is_array()) {
        list = &j[key]["items"];
    } else if (j.is_array()) {
        list = &j;
    }
    if (list == nullptr) return out;
    for (const auto& e : *list) {
        Item it;
        const nlohmann::json* x = &e;
        for (const char* wrap : {"track", "item", "album", "show"}) {
            if (e.is_object() && e.contains(wrap) && e[wrap].is_object() && !e.contains("uri")) {
                x = &e[wrap];
                break;
            }
        }
        if (Parse(*x, it)) out.push_back(std::move(it));
    }
    return out;
}

// --- Asking (cached parsed lists, per address) ----------------------------------------------------

struct Parsed {
    std::vector<std::shared_ptr<const nlohmann::json>> srcs;  // (one per page)
    std::vector<Item> items;
};
std::map<std::string, Parsed> g_parsed;

// Spotify gives long lists in pages (50 at a time) and says where the next one is.
std::string NextPath(const nlohmann::json& j, const char* key) {
    const nlohmann::json* o = &j;
    if (j.is_object() && j.contains(key) && j[key].is_object()) o = &j[key];
    if (!o->is_object()) return {};
    const auto it = o->find("next");
    if (it == o->end() || !it->is_string()) return {};
    const std::string url = it->get<std::string>();
    const std::string prefix = "https://api.spotify.com";
    return url.rfind(prefix, 0) == 0 ? url.substr(prefix.size()) : std::string();
}

// The items at "path" ("key": where the list is in the answer), with ALL its
// pages (up to "maxPages": 10 = 500 playlists...). Returns the state (0
// coming, 1 here, 2 failed) and fills "out" with a pointer to them.
int Items(const std::string& path, const std::vector<Item>*& out, const char* key = "items", int maxAge = 300, int maxPages = 10) {
    Parsed& p = g_parsed[path];
    std::vector<std::shared_ptr<const nlohmann::json>> srcs;
    int first = 0;
    std::string cur = path;
    for (int i = 0; i < maxPages && !cur.empty(); ++i) {
        std::shared_ptr<const nlohmann::json> j;
        const int st = spotify::Browse(cur, j, maxAge);
        if (i == 0) first = st;
        if (!j) break;
        srcs.push_back(j);
        cur = NextPath(*j, key);
    }
    if (srcs != p.srcs) {
        p.srcs = srcs;
        p.items.clear();
        for (const auto& j : srcs) {
            std::vector<Item> page = ParseList(*j, key);
            p.items.insert(p.items.end(), std::make_move_iterator(page.begin()), std::make_move_iterator(page.end()));
        }
    }
    out = &p.items;
    return srcs.empty() ? first : 1;
}

std::shared_ptr<const nlohmann::json> Json(const std::string& path, int* state = nullptr, int maxAge = 300) {
    std::shared_ptr<const nlohmann::json> j;
    const int s = spotify::Browse(path, j, maxAge);
    if (state != nullptr) *state = j ? 1 : s;
    return j;
}

std::string MyId() {
    const auto me = Json("/v1/me", nullptr, 3600);
    return me ? Str(*me, "id") : std::string();
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char hex[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

// --- Playing ------------------------------------------------------------------------------------

// LUMA AS THE PLAYER: when nothing is open to play on, Luma opens its own
// player (spotify_engine.h) and, once its "Luma" device exists, does what
// was asked there.
std::function<void(const std::string&)> g_whenLumaReady;

int g_drawerAsk = -1;  // (the drawer tab to open: Luma's player isn't set up yet)
void ContinueOn(const std::string& deviceId);  // (below)
bool PlayOnLuma(const std::string& body);  // (below)

void WithLumaDevice(std::function<void(const std::string&)> then) {
    namespace eng = luma::launcher::spotifyengine;
    const eng::State st = eng::GetState();
    if (st == eng::State::Ready && !eng::DeviceId().empty()) {
        then(eng::DeviceId());
        return;
    }
    if (st == eng::State::NoProgram) {
        g_drawerAsk = 1;  // (the Devices drawer says why)
        Flash(icons::kAppWindow, T("sp.luma_missing"));
        return;
    }
    if (st == eng::State::Off || st == eng::State::Error) g_drawerAsk = 1;  // (the first time: the browser step shows there)
    g_whenLumaReady = std::move(then);
    eng::Start();
}

// (each frame) runs what was waiting for Luma's player
void LumaPlayerTick() {
    namespace eng = luma::launcher::spotifyengine;
    if (!g_whenLumaReady) return;
    const eng::State st = eng::GetState();
    if (st == eng::State::Ready && !eng::DeviceId().empty()) {
        auto then = std::move(g_whenLumaReady);
        g_whenLumaReady = nullptr;
        then(eng::DeviceId());
    } else if (st == eng::State::Error || st == eng::State::NoProgram) {
        g_whenLumaReady = nullptr;
    }
}

// PLAYS ON LUMA what you were listening to: the same song, from the same
// second, inside the same playlist / album. (Not a "transfer": when nothing
// was playing, there's nothing to transfer and Luma's player is left with an
// empty song.) Nothing known: what you played last, or your Liked songs.
std::string LikedUri();  // (below)
void ContinueOn(const std::string& deviceId) {
    const spotify::Player pl = spotify::GetPlayer();
    nlohmann::json j = nlohmann::json::object();
    if (!pl.trackUri.empty()) {
        if (!pl.contextUri.empty()) {
            j["context_uri"] = pl.contextUri;
            j["offset"] = {{"uri", pl.trackUri}};
        } else {
            j["uris"] = {pl.trackUri};
        }
        j["position_ms"] = static_cast<long long>(spotify::PositionSeconds(pl) * 1000.0);
    } else {
        const std::vector<Item>* recent = nullptr;
        Items("/v1/me/player/recently-played?limit=30", recent, "items", 60, 1);
        if (!LikedUri().empty()) {  // (a context plays best in Luma's player)
            j["context_uri"] = LikedUri();
        } else if (recent != nullptr && !recent->empty()) {
            nlohmann::json uris = nlohmann::json::array();
            for (const Item& it : *recent) {
                if (uris.size() < 30 && it.type == kTrack) uris.push_back(it.uri);
            }
            j["uris"] = uris;
        } else {
            spotify::PlayerTransfer(deviceId);  // (nothing else known)
            return;
        }
    }
    if (!PlayOnLuma(j.dump())) spotify::Send("PUT", "/v1/me/player/play?device_id=" + deviceId, j.dump());
}

// "?device_id=..." when nothing plays now (Spotify needs to know where).
std::string DeviceParam() {
    const spotify::Player pl = spotify::GetPlayer();
    if (pl.hasDevice) return {};
    std::vector<spotify::Device> list;
    std::uint32_t v = 0;
    spotify::Devices(0, list, v);
    for (const auto& d : list) {
        if (d.type == "Computer") return "?device_id=" + d.id;
    }
    return list.empty() ? std::string() : "?device_id=" + list.front().id;
}

// Plays "body" ({"context_uri"...} or {"uris"...}): where it plays now, or
// (nothing open anywhere) in Luma's own player.
// A play body for Luma's own player, as its direct command ("load ...").
bool PlayOnLuma(const std::string& body) {
    namespace eng = luma::launcher::spotifyengine;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object()) return false;
    const long long pos = j.contains("position_ms") && j["position_ms"].is_number() ? j["position_ms"].get<long long>() : 0;
    if (j.contains("context_uri") && j["context_uri"].is_string()) {
        std::string track = "-";
        if (j.contains("offset") && j["offset"].is_object() && j["offset"].contains("uri") && j["offset"]["uri"].is_string()) {
            track = j["offset"]["uri"].get<std::string>();
        }
        eng::Command("load " + j["context_uri"].get<std::string>() + " " + track + " " + std::to_string(pos));
        return true;
    }
    if (j.contains("uris") && j["uris"].is_array() && !j["uris"].empty()) {
        int index = 0;
        if (j.contains("offset") && j["offset"].is_object() && j["offset"].contains("position") && j["offset"]["position"].is_number()) {
            index = j["offset"]["position"].get<int>();
        }
        std::string line = "tracks " + std::to_string(index) + " " + std::to_string(pos);
        for (const auto& u : j["uris"]) {
            if (u.is_string()) line += " " + u.get<std::string>();
        }
        eng::Command(line);
        return true;
    }
    return false;
}

void PlayBody(const std::string& body) {
    namespace eng = luma::launcher::spotifyengine;
    const spotify::Player pl = spotify::GetPlayer();
    // playing on Luma's own player: straight to it
    if (pl.hasDevice && eng::GetState() == eng::State::Ready && pl.device.id == eng::DeviceId() && PlayOnLuma(body)) return;
    if (pl.hasDevice) {
        spotify::Send("PUT", "/v1/me/player/play", body);
        return;
    }
    WithLumaDevice([body](const std::string& id) {
        if (!PlayOnLuma(body)) spotify::Send("PUT", "/v1/me/player/play?device_id=" + id, body);
    });
}

void PlayContext(const std::string& contextUri, const std::string& trackUri = {}) {
    nlohmann::json j = {{"context_uri", contextUri}};
    if (!trackUri.empty()) j["offset"] = {{"uri", trackUri}};
    PlayBody(j.dump());
}

void PlayUris(const std::vector<Item>& list, std::size_t index) {
    // A song with an album: played inside its album (Luma's own player, like
    // Spotify's apps, plays an album or playlist best; a loose list fails there).
    if (index < list.size() && list[index].type == kTrack && !list[index].albumId.empty()) {
        PlayContext("spotify:album:" + list[index].albumId, list[index].uri);
        return;
    }
    nlohmann::json uris = nlohmann::json::array();
    for (std::size_t i = 0; i < list.size() && uris.size() < 100; ++i) {
        if (list[i].type == kTrack || list[i].type == kEpisode) uris.push_back(list[i].uri);
    }
    if (uris.empty()) return;
    const nlohmann::json j = {{"uris", uris}, {"offset", {{"position", static_cast<int>(std::min<std::size_t>(index, uris.size() - 1))}}}};
    PlayBody(j.dump());
}

std::string LikedUri() {
    const std::string me = MyId();
    return me.empty() ? std::string() : "spotify:user:" + me + ":collection";
}

// Plays an album / playlist / artist / show (or your Liked songs) from the start.
void PlayItem(const Item& it, bool shuffle = false) {
    if (shuffle) spotify::PlayerShuffle(true);
    if (it.type == kLiked) {
        PlayContext(LikedUri());
    } else if (it.type == kTrack || it.type == kEpisode) {
        PlayUris({it}, 0);
    } else {
        PlayContext(it.uri);
    }
}

// The web link of an item (to share).
std::string WebLink(const Item& it) {
    const char* kinds[] = {"track", "album", "artist", "playlist", "show", "episode", "collection"};
    if (it.type == kLiked) return "https://open.spotify.com/collection/tracks";
    return "https://open.spotify.com/" + std::string(kinds[it.type]) + "/" + it.id;
}

// --- Navigation ---------------------------------------------------------------------------------

enum PageKind { kTabHome, kTabLibrary, kPagePlaylist, kPageAlbum, kPageArtist, kPageLiked, kPageShow, kPageAll };

struct Page {
    int kind = kTabHome;
    Item item;          // what the page shows (a playlist, album...)
    std::string path;   // "see all": the address of the list
    std::string key = "items";
    std::string title;
    bool round = false;
    float scroll = 0.0f, target = 0.0f, contentH = 0.0f;
    int more = 1;       // how many pages of songs are loaded ("Load more")
    float in = 0.0f;    // 0..1: it comes in
};
std::vector<Page> g_stack = {Page{}};  // the bottom one is a tab
int g_libraryFilter = 0;               // 0 playlists, 1 albums, 2 artists, 3 podcasts

// --- What you searched, and what you opened from it ------------------------------------------------
// Kept in %APPDATA%\Luma\spotify-search.json (only on this PC). With the box
// empty, the search shows them: your last searches (click = search it again)
// and the last results you opened or played (click = again).
std::vector<std::string> g_recentQueries;  // newest first
std::vector<Item> g_recentPicks;           // newest first
bool g_historyLoaded = false;
bool g_recordPicks = false;  // (while drawing the search's results: a click is remembered)
bool g_hasPick = false;
Item g_pick;

fs::path SearchHistoryFile() {
    return DataDir() / L"spotify-search.json";
}

void LoadSearchHistory() {
    if (g_historyLoaded) return;
    g_historyLoaded = true;
    const nlohmann::json j = ReadJson(SearchHistoryFile());
    if (!j.is_object()) return;
    if (j.contains("queries") && j["queries"].is_array()) {
        for (const auto& q : j["queries"]) {
            if (q.is_string()) g_recentQueries.push_back(q.get<std::string>());
        }
    }
    if (j.contains("picks") && j["picks"].is_array()) {
        for (const auto& o : j["picks"]) {
            if (!o.is_object()) continue;
            Item it;
            it.type = o.value("type", static_cast<int>(kTrack));
            it.id = o.value("id", std::string());
            it.uri = o.value("uri", std::string());
            it.name = o.value("name", std::string());
            it.sub = o.value("sub", std::string());
            it.image = o.value("image", std::string());
            it.ms = o.value("ms", 0);
            it.albumId = o.value("albumId", std::string());
            it.albumName = o.value("albumName", std::string());
            it.ownerId = o.value("ownerId", std::string());
            if (o.contains("artists") && o["artists"].is_array()) {
                for (const auto& a : o["artists"]) {
                    if (a.is_array() && a.size() == 2 && a[0].is_string() && a[1].is_string()) {
                        it.artists.emplace_back(a[0].get<std::string>(), a[1].get<std::string>());
                    }
                }
            }
            if (!it.id.empty()) g_recentPicks.push_back(std::move(it));
        }
    }
}

void SaveSearchHistory() {
    nlohmann::json j;
    j["queries"] = g_recentQueries;
    nlohmann::json picks = nlohmann::json::array();
    for (const Item& it : g_recentPicks) {
        nlohmann::json o;
        o["type"] = it.type;
        o["id"] = it.id;
        o["uri"] = it.uri;
        o["name"] = it.name;
        o["sub"] = it.sub;
        o["image"] = it.image;
        o["ms"] = it.ms;
        o["albumId"] = it.albumId;
        o["albumName"] = it.albumName;
        o["ownerId"] = it.ownerId;
        nlohmann::json artists = nlohmann::json::array();
        for (const auto& [id, name] : it.artists) artists.push_back({id, name});
        o["artists"] = artists;
        picks.push_back(o);
    }
    j["picks"] = picks;
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::ofstream(SearchHistoryFile(), std::ios::binary) << j.dump(1);
}

void RememberQuery(const std::string& q) {
    if (q.empty()) return;
    std::erase(g_recentQueries, q);
    g_recentQueries.insert(g_recentQueries.begin(), q);
    if (g_recentQueries.size() > 8) g_recentQueries.resize(8);
    SaveSearchHistory();
}

void RememberPick(const Item& it) {
    if (it.id.empty()) return;
    std::erase_if(g_recentPicks, [&](const Item& o) { return o.id == it.id && o.type == it.type; });
    g_recentPicks.insert(g_recentPicks.begin(), it);
    if (g_recentPicks.size() > 12) g_recentPicks.resize(12);
    SaveSearchHistory();
}

// (Tile and TrackRow call this when you open or play something)
void NotePick(const Item& it) {
    if (!g_recordPicks) return;
    g_pick = it;
    g_hasPick = true;
}

void Open(const Item& it) {
    Page p;
    p.item = it;
    switch (it.type) {
        case kPlaylist: p.kind = kPagePlaylist; break;
        case kAlbum: p.kind = kPageAlbum; break;
        case kArtist: p.kind = kPageArtist; break;
        case kLiked: p.kind = kPageLiked; break;
        case kShow: p.kind = kPageShow; break;
        default: return;
    }
    g_stack.push_back(std::move(p));
}

void OpenAll(const std::string& title, const std::string& path, const char* key, bool round) {
    Page p;
    p.kind = kPageAll;
    p.title = title;
    p.path = path;
    p.key = key;
    p.round = round;
    g_stack.push_back(std::move(p));
}

void OpenArtist(const std::string& id, const std::string& name) {
    Item a;
    a.type = kArtist;
    a.id = id;
    a.uri = "spotify:artist:" + id;
    a.name = name;
    Open(a);
}

void OpenAlbum(const std::string& id, const std::string& name) {
    Item a;
    a.type = kAlbum;
    a.id = id;
    a.uri = "spotify:album:" + id;
    a.name = name;
    Open(a);
}

}  // namespace sp

namespace sp {

// --- Small pieces ------------------------------------------------------------------------------

ImVec2 g_viewMin, g_viewMax;  // the visible part of the page (what's outside isn't drawn or touched)

bool Visible(ImVec2 a, ImVec2 b) {
    return b.y > g_viewMin.y && a.y < g_viewMax.y && b.x > g_viewMin.x && a.x < g_viewMax.x;
}

// A cover (or a placeholder while it downloads). "round" = an artist's circle.
void Cover(ImDrawList* d, const std::string& url, ImVec2 a, ImVec2 b, float rounding, float alpha, bool round = false, int type = kAlbum) {
    const Palette& p = P();
    const std::wstring file = spotify::Image(url);
    const textures::Texture* tex = file.empty() ? nullptr : textures::GetAsync(file);
    const float r = round ? (b.x - a.x) * 0.5f : rounding;
    if (tex != nullptr) {
        d->AddImageRounded(tex->id, a, b, ImVec2(0, 0), ImVec2(1, 1), C(ImVec4(1, 1, 1, 1), alpha), r);
        return;
    }
    if (type == kLiked) {  // Liked songs: Spotify's purple-blue square with a heart
        d->AddRectFilledMultiColor(a, b, C(ImVec4(0.27f, 0.17f, 0.85f, 1), alpha), C(ImVec4(0.55f, 0.75f, 0.95f, 1), alpha),
                                   C(ImVec4(0.55f, 0.75f, 0.95f, 1), alpha), C(ImVec4(0.27f, 0.17f, 0.85f, 1), alpha));
        icons::Draw(d, icons::kHeartFilled, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), std::round((b.x - a.x) * 0.36f),
                    C(ImVec4(1, 1, 1, 1), alpha));
        return;
    }
    d->AddRectFilled(a, b, C(Mix(p.card, ImVec4(1, 1, 1, 1), 0.05f), alpha), r);
    icons::Draw(d, type == kArtist ? icons::kPerson : icons::kMusic2, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                std::round((b.x - a.x) * 0.3f), C(p.textDim, 0.6f * alpha), S(1.5f));
}

// The green play button (on a tile, on a page's header).
bool PlayButton(ImDrawList* d, const char* id, ImVec2 c, float r, float alpha, bool playing = false) {
    float hover = 0.0f;
    const bool clicked = Hit(id, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), &hover);
    const float press = Anim(ImGui::GetID(id) + 3, ImGui::IsItemActive(), 22.0f);
    const float rr = r * (1.0f + 0.06f * hover - 0.06f * press);
    d->AddCircleFilled(ImVec2(c.x, c.y + S(3.0f)), rr, C(ImVec4(0, 0, 0, 1), 0.25f * alpha), 32);
    d->AddCircleFilled(c, rr, C(Mix(kSpotifyGreen, ImVec4(1, 1, 1, 1), 0.10f * hover), alpha), 32);
    icons::Draw(d, playing ? icons::kPauseFilled : icons::kPlayFilled, ImVec2(c.x + (playing ? 0.0f : r * 0.06f), c.y), std::round(r * 0.9f),
                C(ImVec4(0.02f, 0.02f, 0.02f, 1), alpha));
    return clicked;
}

// --- The options menu (right click / "...") ---------------------------------------------------------

// SPOTIFY'S RADIO of a song / artist / album / playlist: Luma's player asks
// Spotify for it (the same "Radio" playlist Spotify's app makes), then its
// page opens and it plays.
std::string g_radioSeed, g_radioTitle;
double g_radioAskedAt = 0.0;
std::string g_radioPlayId;  // the radio playlist to play once its songs are here
double g_radioPlayAt = 0.0;

// Plays the radio: on Luma's own player as its list of songs (its "context"
// sometimes comes empty to librespot), elsewhere as the playlist.
void RadioPlayTick() {
    if (g_radioPlayId.empty()) return;
    namespace eng = luma::launcher::spotifyengine;
    const std::string uri = "spotify:playlist:" + g_radioPlayId;
    const spotify::Player pl = spotify::GetPlayer();
    const bool luma = !pl.hasDevice || (eng::GetState() == eng::State::Ready && pl.device.id == eng::DeviceId());
    if (!luma) {
        PlayContext(uri);
        g_radioPlayId.clear();
        return;
    }
    std::shared_ptr<const nlohmann::json> j;
    const int st = eng::PlaylistSongs(g_radioPlayId, j);
    if (st == 1 && j) {
        const std::vector<Item> songs = ParseList(*j, "items");
        if (!songs.empty()) {
            nlohmann::json uris = nlohmann::json::array();
            for (const Item& t : songs) {
                if (uris.size() < 100) uris.push_back(t.uri);
            }
            PlayBody(nlohmann::json({{"uris", uris}}).dump());
        } else {
            PlayContext(uri);
        }
        g_radioPlayId.clear();
    } else if (st == 2 || ImGui::GetTime() - g_radioPlayAt > 20.0) {
        PlayContext(uri);
        g_radioPlayId.clear();
    }
}

void StartRadio(const Item& it) {
    g_radioSeed = it.uri;
    g_radioTitle = std::string(T("sp.radio_of")) + " " + it.name;
    g_radioAskedAt = ImGui::GetTime();
    std::shared_ptr<const nlohmann::json> j;
    luma::launcher::spotifyengine::Ask("radio " + it.uri, j, true);
    Flash(icons::kSignal, T("sp.radio_looking"));
}

void RadioTick() {
    if (g_radioSeed.empty()) return;
    std::shared_ptr<const nlohmann::json> j;
    const int st = luma::launcher::spotifyengine::Ask("radio " + g_radioSeed, j);
    if (st == 0 && ImGui::GetTime() - g_radioAskedAt < 25.0) return;
    if (st == 1 && j && j->contains("uri") && (*j)["uri"].is_string()) {
        const std::string uri = (*j)["uri"].get<std::string>();
        Item radio;
        radio.type = kPlaylist;
        radio.uri = uri;
        radio.id = uri.substr(uri.find_last_of(':') + 1);
        radio.name = g_radioTitle;
        radio.sub = "Spotify";
        Open(radio);
        g_radioPlayId = radio.id;  // (played once its songs are read: RadioPlayTick)
        g_radioPlayAt = ImGui::GetTime();
        Flash(icons::kSignal, T("sp.radio_started"));
    } else {
        Flash(icons::kInfo, T("sp.radio_failed"));
    }
    g_radioSeed.clear();
}

struct Menu {
    bool open = false;
    float anim = 0.0f;
    ImVec2 pos;
    Item item;
    int sub = 0;  // 0 the options, 1 "add to a playlist"
    double copiedAt = -10.0;
    int openedFrame = -1;  // (the click that opened it doesn't also close it)
};
Menu g_menu;

void OpenMenu(const Item& it) {
    g_menu.open = true;
    g_menu.item = it;
    g_menu.sub = 0;
    g_menu.pos = ImGui::GetIO().MousePos;
    g_menu.openedFrame = ImGui::GetFrameCount();
}

// The artists of a playlist (the 5 that appear most), for its radio.
std::vector<std::string> PlaylistArtists(const std::string& playlistId, bool* ready) {
    const std::vector<Item>* list = nullptr;
    const int state = Items("/v1/playlists/" + playlistId, list, "items", 300, 30);
    *ready = state == 1;
    std::map<std::string, int> count;
    if (list != nullptr) {
        for (const Item& t : *list) {
            for (const auto& a : t.artists) ++count[a.first];
        }
    }
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [id, n] : count) sorted.emplace_back(-n, id);
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::string> out;
    for (std::size_t i = 0; i < sorted.size() && out.size() < 5; ++i) out.push_back(sorted[i].second);
    return out;
}

void DrawMenu(ImDrawList* d, ImVec2 display) {
    Menu& m = g_menu;
    m.anim += ((m.open ? 1.0f : 0.0f) - m.anim) * std::min(1.0f, Dt() * 18.0f);
    if (m.anim < 0.01f) return;
    const Palette& p = P();
    const Item& it = m.item;
    struct Opt {
        const char* id;
        const char* icon;
        std::string label;
        int action;
        bool enabled = true;
    };
    std::vector<Opt> opts;
    bool likedKnown = false, liked = false;
    if (m.sub == 0) {
        opts.push_back({"##m_play", icons::kPlayFilled, T("sp.play"), 1});
        if (it.type == kTrack || it.type == kEpisode) opts.push_back({"##m_queue", icons::kList, T("sp.add_queue"), 2});
        if (it.type == kAlbum || it.type == kPlaylist || it.type == kLiked) opts.push_back({"##m_shuffle", icons::kShuffle, T("sp.shuffle_play"), 3});
        if (it.type == kTrack) opts.push_back({"##m_radio", icons::kSignal, T("sp.radio_song"), 4});
        if (it.type == kArtist) opts.push_back({"##m_radio", icons::kSignal, T("sp.radio_artist"), 4});
        if (it.type == kAlbum) opts.push_back({"##m_radio", icons::kSignal, T("sp.radio_album"), 4});
        if (it.type == kPlaylist) opts.push_back({"##m_radio", icons::kSignal, T("sp.radio_playlist"), 4});
        if (it.type == kTrack || it.type == kAlbum) {
            for (std::size_t i = 0; i < it.artists.size() && i < 2; ++i) {
                opts.push_back({i == 0 ? "##m_artist0" : "##m_artist1", icons::kPerson, std::string(T("sp.go_artist")) + " " + it.artists[i].second,
                                10 + static_cast<int>(i)});
            }
        }
        if (it.type == kTrack && !it.albumId.empty()) opts.push_back({"##m_album", icons::kLayers, T("sp.go_album"), 5});
        if (it.type == kTrack) {
            opts.push_back({"##m_toplaylist", icons::kPlus, T("sp.add_playlist"), 6});
            std::shared_ptr<const nlohmann::json> c = Json("/v1/me/library/contains?uris=" + it.uri, nullptr, 20);
            if (c && c->is_array() && !c->empty() && (*c)[0].is_boolean()) {
                likedKnown = true;
                liked = (*c)[0].get<bool>();
            }
            opts.push_back({"##m_like", liked ? icons::kHeartFilled : icons::kHeart, liked ? T("sp.unlike") : T("sp.like"), 7, likedKnown});
        }
        const bool copied = ImGui::GetTime() - m.copiedAt < 1.5;
        opts.push_back({"##m_copy", copied ? kIconCheck : icons::kCopy, copied ? T("opt.copied") : T("sp.copy_link"), 8});
        opts.push_back({"##m_open", icons::kAppWindow, T("sp.open_spotify"), 9});
    } else {
        // your own playlists (and the ones you can add to)
        opts.push_back({"##m_back", icons::kBack, T("sp.add_playlist"), 20});
        const std::vector<Item>* lists = nullptr;
        Items("/v1/me/playlists?limit=50", lists);
        const std::string me = MyId();
        int n = 0;
        if (lists != nullptr) {
            for (const Item& pl : *lists) {
                if (pl.ownerId != me || n >= 14) continue;
                static char ids[16][24];
                std::snprintf(ids[n], sizeof(ids[n]), "##m_pl%d", n);
                opts.push_back({ids[n], icons::kMusic2, pl.name, 100 + n});
                ++n;
            }
        }
        if (n == 0) opts.push_back({"##m_none", icons::kInfo, T("sp.no_own_playlists"), 0, false});
    }
    const float itemH = S(38.0f), width = S(290.0f);
    const float height = itemH * static_cast<float>(opts.size()) + S(12.0f);
    ImVec2 min(m.pos.x + S(4.0f), m.pos.y + S(4.0f));
    if (min.x + width > display.x - S(10.0f)) min.x = m.pos.x - width - S(4.0f);
    if (min.y + height > display.y - S(10.0f)) min.y = std::max(S(10.0f), display.y - S(10.0f) - height);
    const ImVec2 max(min.x + width, min.y + height);
    const int start = d->VtxBuffer.Size;
    d->AddRectFilled(ImVec2(min.x, min.y + S(6.0f)), ImVec2(max.x, max.y + S(8.0f)), Col(ImVec4(0, 0, 0, 1), 0.28f), S(14.0f));
    d->AddRectFilled(min, max, Col(Mix(p.card, ImVec4(1, 1, 1, 1), 0.04f), 0.98f), S(14.0f));
    d->AddRect(min, max, Col(ImVec4(1, 1, 1, 1), 0.12f), S(14.0f));
    float y = min.y + S(6.0f);
    int clicked = -1;
    for (const Opt& o : opts) {
        const ImVec2 a(min.x + S(6.0f), y), b(max.x - S(6.0f), y + itemH);
        float hover = 0.0f;
        if (Hit(o.id, a, b, &hover, o.enabled && m.open)) clicked = o.action;
        d->AddRectFilled(a, b, Col(p.text, 0.07f * hover), S(9.0f));
        const float al = o.enabled ? 1.0f : 0.4f;
        const bool green = (o.action == 7 && liked) || o.action == 1;
        icons::Draw(d, o.icon, ImVec2(a.x + S(18.0f), (a.y + b.y) * 0.5f), S(16.0f), Col(green ? kSpotifyGreen : p.textDim, al), S(1.6f));
        TextFit(d, g_fonts.semibold, ImVec2(a.x + S(38.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f), Col(p.text, al), o.label.c_str(),
                b.x - a.x - S(46.0f));
        if (o.action == 6) icons::Draw(d, icons::kChevronRight, ImVec2(b.x - S(14.0f), (a.y + b.y) * 0.5f), S(14.0f), Col(p.textDim), S(1.6f));
        y += itemH;
    }
    bool close = false;
    if (clicked >= 0) {
        close = true;
        if (clicked == 1) {
            PlayItem(it);
        } else if (clicked == 2) {
            spotify::Send("POST", "/v1/me/player/queue?uri=" + UrlEncode(it.uri));
            spotify::Forget("/v1/me/player/queue");
            Flash(icons::kList, T("sp.added_queue"));
        } else if (clicked == 3) {
            PlayItem(it, true);
        } else if (clicked == 4) {
            StartRadio(it);
        } else if (clicked == 5) {
            OpenAlbum(it.albumId, it.albumName);
        } else if (clicked == 10 || clicked == 11) {
            const auto& a = it.artists[static_cast<std::size_t>(clicked - 10)];
            OpenArtist(a.first, a.second);
        } else if (clicked == 6) {
            m.sub = 1;
            close = false;
        } else if (clicked == 20) {
            m.sub = 0;
            close = false;
        } else if (clicked == 7) {
            spotify::Send(liked ? "DELETE" : "PUT", "/v1/me/library?uris=" + UrlEncode(it.uri));
            spotify::Forget("/v1/me/library/contains?uris=" + it.uri);
            spotify::Forget("/v1/me/tracks");
            const spotify::Player pl = spotify::GetPlayer();
            if (pl.trackId == it.id) spotify::PlayerLike(!liked);
            Flash(liked ? icons::kHeart : icons::kHeartFilled, liked ? T("sp.unliked") : T("sp.liked"));
        } else if (clicked == 8) {
            ImGui::SetClipboardText(WebLink(it).c_str());
            m.copiedAt = ImGui::GetTime();
            close = false;
        } else if (clicked == 9) {
            const std::wstring uri = std::wstring(it.uri.begin(), it.uri.end());
            ShellExecuteW(nullptr, L"open", it.type == kLiked ? L"spotify:collection:tracks" : uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (clicked >= 100) {
            const std::vector<Item>* lists = nullptr;
            Items("/v1/me/playlists?limit=50", lists);
            const std::string me = MyId();
            int n = 0;
            if (lists != nullptr) {
                for (const Item& pl : *lists) {
                    if (pl.ownerId != me) continue;
                    if (n++ == clicked - 100) {
                        const nlohmann::json body = {{"uris", {it.uri}}};
                        spotify::Send("POST", "/v1/playlists/" + pl.id + "/items", body.dump());
                        spotify::Forget("/v1/playlists/" + pl.id);
                        std::string text = std::string(T("sp.added_to")) + " " + pl.name;
                        Flash(icons::kPlus, text.c_str());
                        break;
                    }
                }
            }
        }
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool inside = mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y;
    const bool clickOutside = ImGui::GetFrameCount() > m.openedFrame &&
                              (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) && !inside;
    if (m.open && (close || clickOutside || ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        m.open = false;
    }
    const float e = EaseOutCubic(m.anim);
    AnimateSince(d, start, e, ImVec2(0.0f, (1.0f - e) * S(-6.0f)));
}

// --- Tiles and rows -------------------------------------------------------------------------------

// A tile (cover + name + what it is). Click = open (a song: play); right
// click or "..." = options; the green button on hover = play.
void Tile(ImDrawList* d, const char* id, const Item& it, ImVec2 pos, float w, float alpha, const std::vector<Item>* list = nullptr,
          std::size_t index = 0) {
    const Palette& p = P();
    const bool round = it.type == kArtist;
    const float h = w + g_fonts.semibold->FontSize + g_fonts.tiny->FontSize + S(14.0f);
    if (!Visible(pos, ImVec2(pos.x + w, pos.y + h))) return;
    float hover = 0.0f;
    ImGui::SetNextItemAllowOverlap();  // (its play button, on top, can be pressed)
    const bool clicked = Hit(id, pos, ImVec2(pos.x + w, pos.y + h), &hover);
    // (still "hovered" while the mouse is on its play button)
    const bool over = !g_blockInput && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    hover = Anim(ImGui::GetID(id) + 50, over);
    const bool right = over && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const float lift = S(4.0f) * hover;
    const ImVec2 a(pos.x, pos.y - lift), b(pos.x + w, pos.y + w - lift);
    if (hover > 0.01f) d->AddRectFilled(ImVec2(a.x - S(8.0f), a.y - S(8.0f)), ImVec2(b.x + S(8.0f), pos.y + h + S(4.0f) - lift),
                                        C(ImVec4(1, 1, 1, 1), 0.06f * hover * alpha), S(12.0f));
    Cover(d, it.image, a, b, S(10.0f), alpha, round, it.type);
    float ty = b.y + S(8.0f);
    TextFit(d, g_fonts.semibold, ImVec2(pos.x, ty), C(p.text, alpha), it.name.c_str(), w);
    ty += g_fonts.semibold->FontSize + S(2.0f);
    TextFit(d, g_fonts.tiny, ImVec2(pos.x, ty), C(p.textDim, alpha), it.sub.c_str(), w);
    // the play button, rising in on hover
    bool played = false;
    if (hover > 0.05f) {
        char pid[48];
        std::snprintf(pid, sizeof(pid), "%s_play", id);
        const float r = S(20.0f);
        const ImVec2 c(b.x - r - S(8.0f), b.y - r - S(8.0f) + (1.0f - hover) * S(10.0f));
        if (PlayButton(d, pid, c, r, hover * alpha)) {
            played = true;
            if ((it.type == kTrack || it.type == kEpisode) && list != nullptr) {
                PlayUris(*list, index);
            } else {
                PlayItem(it);
            }
        }
    }
    if (right) OpenMenu(it);
    if (played || clicked) NotePick(it);
    if (clicked && !played) {
        if (it.type == kTrack || it.type == kEpisode) {
            if (list != nullptr) PlayUris(*list, index);
        } else {
            Open(it);
        }
    }
}

// A row of tiles with its title; as many as fit, and "See all".
float Row(ImDrawList* d, const char* id, const char* title, const std::vector<Item>& items, ImVec2 pos, float width, float alpha,
          const std::string& allPath, const char* allKey, const Item* first = nullptr) {
    const Palette& p = P();
    if (items.empty() && first == nullptr) return 0.0f;
    const float tile = std::clamp(width / 6.5f, S(130.0f), S(180.0f));
    const float gap = S(18.0f);
    const int fit = std::max(1, static_cast<int>((width + gap) / (tile + gap)));
    Text(d, g_fonts.title, pos, C(p.text, alpha), title);
    if (!allPath.empty()) {
        const char* all = T("sp.see_all");
        const ImVec2 as = TextSize(g_fonts.semibold, all);
        const ImVec2 a(pos.x + width - as.x, pos.y + S(4.0f)), b(pos.x + width, pos.y + S(4.0f) + as.y);
        char aid[48];
        std::snprintf(aid, sizeof(aid), "%s_all", id);
        float hover = 0.0f;
        if (Visible(a, b) && Hit(aid, a, b, &hover)) OpenAll(title, allPath, allKey, items.empty() ? false : items[0].type == kArtist);
        Text(d, g_fonts.semibold, a, C(Mix(p.textDim, p.text, hover), alpha), all);
    }
    const float y = pos.y + g_fonts.title->FontSize + S(14.0f);
    int shown = 0;
    float x = pos.x;
    if (first != nullptr) {
        char tid[48];
        std::snprintf(tid, sizeof(tid), "%s_first", id);
        Tile(d, tid, *first, ImVec2(x, y), tile, alpha);
        x += tile + gap;
        ++shown;
    }
    for (std::size_t i = 0; i < items.size() && shown < fit; ++i, ++shown) {
        char tid[48];
        std::snprintf(tid, sizeof(tid), "%s_%zu", id, i);
        Tile(d, tid, items[i], ImVec2(x, y), tile, alpha, &items, i);
        x += tile + gap;
    }
    return g_fonts.title->FontSize + S(14.0f) + tile + g_fonts.semibold->FontSize + g_fonts.tiny->FontSize + S(40.0f);
}

// A grid of tiles (Library, "see all").
float Grid(ImDrawList* d, const char* id, const std::vector<Item>& items, ImVec2 pos, float width, float alpha, const Item* first = nullptr) {
    const float tile = std::clamp(width / 6.5f, S(130.0f), S(180.0f));
    const float gap = S(18.0f);
    const int cols = std::max(1, static_cast<int>((width + gap) / (tile + gap)));
    const float rowH = tile + g_fonts.semibold->FontSize + g_fonts.tiny->FontSize + S(34.0f);
    int n = 0;
    auto place = [&](const Item& it, const std::vector<Item>* list, std::size_t index) {
        char tid[48];
        std::snprintf(tid, sizeof(tid), "%s_%d", id, n);
        Tile(d, tid, it, ImVec2(pos.x + static_cast<float>(n % cols) * (tile + gap), pos.y + static_cast<float>(n / cols) * rowH), tile, alpha, list,
             index);
        ++n;
    };
    if (first != nullptr) place(*first, nullptr, 0);
    for (std::size_t i = 0; i < items.size(); ++i) place(items[i], &items, i);
    return static_cast<float>((n + cols - 1) / cols) * rowH;
}

// A song in a list: number (or the equalizer when it plays), cover, name,
// artists, album, length, and "..." on hover. Click = play it here.
void TrackRow(ImDrawList* d, const char* id, const Item& it, int number, ImVec2 pos, float width, float alpha, bool showAlbum, bool showCover,
              const std::function<void()>& play) {
    const Palette& p = P();
    const float h = S(54.0f);
    const ImVec2 a(pos.x, pos.y), b(pos.x + width, pos.y + h - S(4.0f));
    if (!Visible(a, b)) return;
    float hover = 0.0f;
    ImGui::SetNextItemAllowOverlap();  // (its "..." button, on top, can be pressed)
    const bool clicked = Hit(id, a, b, &hover);
    const bool over = !g_blockInput && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    hover = Anim(ImGui::GetID(id) + 50, over);
    const bool right = over && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const spotify::Player pl = spotify::GetPlayer();
    const bool current = !pl.trackId.empty() && pl.trackId == it.id;
    d->AddRectFilled(a, b, C(ImVec4(1, 1, 1, 1), (0.07f * hover + (current ? 0.05f : 0.0f)) * alpha), S(10.0f));
    const float cy = (a.y + b.y) * 0.5f;
    // number / playing
    if (current && pl.playing) {
        for (int k = 0; k < 3; ++k) {
            const float v = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * (6.0f + k) + k));
            const float bh = S(12.0f) * v;
            const float bx = a.x + S(14.0f) + static_cast<float>(k) * S(5.0f);
            d->AddRectFilled(ImVec2(bx, cy + S(6.0f) - bh), ImVec2(bx + S(3.0f), cy + S(6.0f)), C(kSpotifyGreen, alpha), S(1.0f));
        }
    } else if (hover > 0.5f) {
        icons::Draw(d, icons::kPlayFilled, ImVec2(a.x + S(20.0f), cy), S(14.0f), C(p.text, alpha));
    } else if (number > 0) {
        char num[8];
        std::snprintf(num, sizeof(num), "%d", number);
        const ImVec2 ns = TextSize(g_fonts.semibold, num);
        Text(d, g_fonts.semibold, ImVec2(a.x + S(20.0f) - ns.x * 0.5f, cy - ns.y * 0.5f), C(p.textDim, alpha), num);
    }
    float x = a.x + S(44.0f);
    if (showCover) {
        Cover(d, it.image, ImVec2(x, cy - S(20.0f)), ImVec2(x + S(40.0f), cy + S(20.0f)), S(6.0f), alpha, false, it.type);
        x += S(52.0f);
    }
    const float right0 = b.x - S(90.0f);
    const float albumX = showAlbum ? a.x + width * 0.58f : right0;
    TextFit(d, g_fonts.semibold, ImVec2(x, cy - g_fonts.semibold->FontSize - S(1.0f)), C(current ? kSpotifyGreen : p.text, alpha), it.name.c_str(),
            albumX - x - S(16.0f));
    TextFit(d, g_fonts.tiny, ImVec2(x, cy + S(2.0f)), C(p.textDim, alpha), it.sub.c_str(), albumX - x - S(16.0f));
    if (showAlbum) TextFit(d, g_fonts.tiny, ImVec2(albumX, cy - g_fonts.tiny->FontSize * 0.5f), C(p.textDim, alpha), it.albumName.c_str(), right0 - albumX - S(10.0f));
    if (it.ms > 0) {
        char len[16];
        FormatTime(it.ms / 1000.0, len, sizeof(len));
        const ImVec2 ls = TextSize(g_fonts.tiny, len);
        Text(d, g_fonts.tiny, ImVec2(b.x - S(52.0f) - ls.x, cy - ls.y * 0.5f), C(p.textDim, alpha), len);
    }
    // "..." (on hover)
    bool menu = right;
    if (hover > 0.05f) {
        char mid[48];
        std::snprintf(mid, sizeof(mid), "%s_more", id);
        float mh = 0.0f;
        const ImVec2 mc(b.x - S(22.0f), cy);
        if (Hit(mid, ImVec2(mc.x - S(14.0f), mc.y - S(14.0f)), ImVec2(mc.x + S(14.0f), mc.y + S(14.0f)), &mh)) menu = true;
        icons::Draw(d, icons::kEllipsis, mc, S(18.0f), C(p.text, (0.6f + 0.4f * mh) * hover * alpha), S(1.8f));
    }
    if (menu) {
        OpenMenu(it);
    } else if (clicked) {
        NotePick(it);
        play();
    }
}

}  // namespace sp

namespace sp {

// --- The tabs -----------------------------------------------------------------------------------

Item LikedItem() {
    Item it;
    it.type = kLiked;
    it.name = T("sp.liked_songs");
    it.sub = T("sp.playlist");
    it.uri = "liked";
    return it;
}

float HomeTab(ImDrawList* d, ImVec2 pos, float width, float alpha) {
    float y = pos.y;
    const std::vector<Item>* list = nullptr;
    // played lately (without repeats)
    if (Items("/v1/me/player/recently-played?limit=30", list, "items", 60, 1) == 1 && list != nullptr) {
        static std::vector<Item> unique;
        static const std::vector<Item>* from = nullptr;
        static std::size_t fromSize = 0;
        if (from != list || fromSize != list->size()) {
            from = list;
            fromSize = list->size();
            unique.clear();
            for (const Item& it : *list) {
                bool seen = false;
                for (const Item& u : unique) seen = seen || u.id == it.id;
                if (!seen) unique.push_back(it);
            }
        }
        y += Row(d, "##r_recent", T("sp.recent"), unique, ImVec2(pos.x, y), width, alpha, {}, nullptr);
    }
    if (Items("/v1/me/playlists?limit=50", list) >= 0 && list != nullptr) {
        const Item liked = LikedItem();
        y += Row(d, "##r_lists", T("sp.your_playlists"), *list, ImVec2(pos.x, y), width, alpha, "/v1/me/playlists?limit=50", "items", &liked);
    }
    if (Items("/v1/me/top/artists?limit=30&time_range=short_term", list) == 1 && list != nullptr) {
        y += Row(d, "##r_topa", T("sp.top_artists"), *list, ImVec2(pos.x, y), width, alpha, "/v1/me/top/artists?limit=30&time_range=short_term", "items");
    }
    if (Items("/v1/me/top/tracks?limit=30&time_range=short_term", list) == 1 && list != nullptr) {
        y += Row(d, "##r_topt", T("sp.top_tracks"), *list, ImVec2(pos.x, y), width, alpha, "/v1/me/top/tracks?limit=30&time_range=short_term", "items");
    }
    if (Items("/v1/me/albums?limit=30", list) == 1 && list != nullptr) {
        y += Row(d, "##r_albums", T("sp.saved_albums"), *list, ImVec2(pos.x, y), width, alpha, "/v1/me/albums?limit=50", "items");
    }
    if (y == pos.y) Spinner(d, ImVec2(pos.x + width * 0.5f, pos.y + S(80.0f)), S(16.0f), C(kSpotifyGreen, alpha));
    return y - pos.y;
}

float LibraryTab(ImDrawList* d, ImVec2 pos, float width, float alpha) {
    const char* labels[4] = {T("sp.playlists"), T("sp.albums"), T("sp.artists"), T("sp.podcasts")};
    Segmented("##sp_libfilter", pos, labels, 4, &g_libraryFilter);
    const float y = pos.y + S(56.0f);
    const std::vector<Item>* list = nullptr;
    static const char* paths[4] = {"/v1/me/playlists?limit=50", "/v1/me/albums?limit=50", "/v1/me/following?type=artist&limit=50",
                                   "/v1/me/shows?limit=50"};
    const char* key = g_libraryFilter == 2 ? "artists" : "items";
    const int state = Items(paths[g_libraryFilter], list, key);
    if (state == 0 || list == nullptr) {
        Spinner(d, ImVec2(pos.x + width * 0.5f, y + S(60.0f)), S(16.0f), C(kSpotifyGreen, alpha));
        return S(160.0f);
    }
    const Item liked = LikedItem();
    return S(56.0f) + Grid(d, "##g_lib", *list, ImVec2(pos.x, y), width, alpha, g_libraryFilter == 0 ? &liked : nullptr);
}

// --- A page with songs (playlist, album, Liked songs, a podcast) ----------------------------------

float SongsPage(ImDrawList* d, Page& pg, ImVec2 pos, float width, float alpha) {
    const Palette& p = P();
    Item& it = pg.item;
    // its header: the cover, what it is, the name, details, play / shuffle / radio / ...
    std::string headPath, listPath, listKey = "items";
    int pageSize = 100;
    if (pg.kind == kPagePlaylist) {
        // (Spotify's API since 2026: a playlist's songs come inside the playlist
        // itself, "items", page after page; the old ".../tracks" answers 403.
        // And only for YOUR playlists: other people's come without songs.)
        headPath = "/v1/playlists/" + it.id;
        listPath = headPath;
    } else if (pg.kind == kPageAlbum) {
        headPath = "/v1/albums/" + it.id;
        listPath = headPath;
        listKey = "tracks";
    } else if (pg.kind == kPageLiked) {
        listPath = "/v1/me/tracks?limit=50";
        pageSize = 50;
    } else if (pg.kind == kPageShow) {
        headPath = "/v1/shows/" + it.id;
        listPath = "/v1/shows/" + it.id + "/episodes?limit=50";
        pageSize = 50;
    }
    if (!headPath.empty()) {
        if (auto h = Json(headPath)) {
            Item full;
            if (Parse(*h, full)) {
                if (it.image.empty()) it.image = full.image;
                if (it.name.empty()) it.name = full.name;
                if (full.total > 0) it.total = full.total;
                if (it.sub.empty()) it.sub = full.sub;
                if (it.artists.empty()) it.artists = full.artists;
                if (it.ownerId.empty()) it.ownerId = full.ownerId;
            }
        }
    }
    const float cover = S(190.0f);
    Cover(d, it.image, pos, ImVec2(pos.x + cover, pos.y + cover), S(12.0f), alpha, false, it.type);
    const float hx = pos.x + cover + S(28.0f);
    const char* kinds[] = {"", T("sp.album"), "", T("sp.playlist"), T("sp.podcast"), "", T("sp.playlist")};
    Text(d, g_fonts.tiny, ImVec2(hx, pos.y + S(40.0f)), C(p.textDim, alpha), kinds[it.type]);
    TextFit(d, g_fonts.hero, ImVec2(hx, pos.y + S(56.0f)), C(p.text, alpha), it.name.c_str(), width - cover - S(40.0f));
    {
        std::string line = it.sub;
        if (it.total > 0) {
            char n[48];
            std::snprintf(n, sizeof(n), T("sp.n_songs"), it.total);
            line += line.empty() ? n : std::string("  ·  ") + n;
        }
        TextFit(d, g_fonts.regular, ImVec2(hx, pos.y + S(64.0f) + g_fonts.hero->FontSize), C(p.textDim, alpha), line.c_str(),
                width - cover - S(40.0f));
    }
    const float by = pos.y + cover - S(26.0f);
    const spotify::Player pl = spotify::GetPlayer();
    if (PlayButton(d, "##sp_pg_play", ImVec2(hx + S(26.0f), by), S(26.0f), alpha)) PlayItem(it);
    if (IconToggle(d, "##sp_pg_shuffle", ImVec2(hx + S(82.0f), by), icons::kShuffle, S(20.0f), false)) PlayItem(it, true);
    if (it.type == kPlaylist || it.type == kAlbum) {
        if (IconToggle(d, "##sp_pg_radio", ImVec2(hx + S(126.0f), by), icons::kSignal, S(20.0f), false)) StartRadio(it);
    }
    if (IconToggle(d, "##sp_pg_more", ImVec2(hx + S(170.0f), by), icons::kEllipsis, S(20.0f), false)) OpenMenu(it);
    float y = pos.y + cover + S(30.0f);
    // the songs (more pages with "Load more")
    std::vector<Item> songs;
    bool loading = false, more = false;
    for (int page = 0; page < pg.more; ++page) {
        std::string path = listPath;
        if (page > 0) path += (path.find('?') == std::string::npos ? "?offset=" : "&offset=") + std::to_string(page * pageSize);
        const std::vector<Item>* list = nullptr;
        const int state = Items(path, list, listKey.c_str(), 300, pg.kind == kPagePlaylist ? 30 : 1);
        if (state == 0) loading = true;
        if (list != nullptr) {
            for (const Item& s : *list) {
                Item copy = s;
                if (pg.kind == kPageAlbum) {  // (an album's songs don't carry its cover)
                    copy.image = it.image;
                    copy.albumId = it.id;
                    copy.albumName = it.name;
                }
                songs.push_back(std::move(copy));
            }
            more = static_cast<int>(list->size()) >= pageSize && pg.kind != kPageAlbum && pg.kind != kPagePlaylist;
        }
    }
    // Other people's playlists: the public API gives them without songs, so
    // they come through Luma's own player (Spotify's internal channel).
    bool viaPlayer = false;
    if (pg.kind == kPagePlaylist && songs.empty() && !loading) {
        std::shared_ptr<const nlohmann::json> pj;
        const int st = luma::launcher::spotifyengine::PlaylistSongs(it.id, pj);
        if (st == 1 && pj) {
            static std::map<std::string, std::vector<Item>> parsed;
            auto& v = parsed[it.id];
            if (v.empty()) v = ParseList(*pj, "items");
            songs = v;
            viaPlayer = true;
        } else if (st == 0) {
            loading = true;
        }
    }
    (void)viaPlayer;
    if (it.image.empty() && !songs.empty()) it.image = songs.front().image;  // (no cover of its own: its first song's)
    const bool showAlbum = pg.kind != kPageAlbum && pg.kind != kPageShow && width > S(700.0f);
    for (std::size_t i = 0; i < songs.size(); ++i) {
        char rid[32];
        std::snprintf(rid, sizeof(rid), "##sp_song%zu", i);
        const Item& s = songs[i];
        TrackRow(d, rid, s, static_cast<int>(i) + 1, ImVec2(pos.x, y), width, alpha, showAlbum, pg.kind != kPageAlbum, [&, i]() {
            if (pg.kind == kPageLiked) {
                PlayContext(LikedUri(), songs[i].uri);
            } else if (pg.kind == kPageShow) {
                PlayUris(songs, i);
            } else {
                PlayContext(it.uri, songs[i].uri);
            }
        });
        y += S(54.0f);
    }
    if (songs.empty() && !loading) {
        TextWrap(d, g_fonts.regular, ImVec2(pos.x, y + S(10.0f)), C(p.textDim, alpha), T("sp.list_unavailable"), width);
        y += S(60.0f);
    }
    if (loading) {
        Spinner(d, ImVec2(pos.x + width * 0.5f, y + S(24.0f)), S(14.0f), C(kSpotifyGreen, alpha));
        y += S(50.0f);
    } else if (more) {
        const char* label = T("sp.load_more");
        const float lw = TextSize(g_fonts.semibold, label).x + S(28.0f);
        float bw = 0.0f;
        const ImVec2 bp(pos.x + width * 0.5f - lw * 0.5f, y + S(10.0f));
        if (Visible(bp, ImVec2(bp.x + lw, bp.y + S(34.0f))) && SmallPill(d, "##sp_more", bp, nullptr, label, false, &bw)) ++pg.more;
        y += S(60.0f);
    }
    (void)pl;
    return y - pos.y;
}

float ArtistPage(ImDrawList* d, Page& pg, ImVec2 pos, float width, float alpha) {
    const Palette& p = P();
    Item& it = pg.item;
    int followers = 0;
    if (auto h = Json("/v1/artists/" + it.id)) {
        Item full;
        if (Parse(*h, full)) {
            if (it.image.empty()) it.image = full.image;
            if (it.name.empty()) it.name = full.name;
        }
        if (h->contains("followers") && (*h)["followers"].is_object()) followers = (*h)["followers"].value("total", 0);
    }
    const float cover = S(190.0f);
    Cover(d, it.image, pos, ImVec2(pos.x + cover, pos.y + cover), 0.0f, alpha, true, kArtist);
    const float hx = pos.x + cover + S(28.0f);
    Text(d, g_fonts.tiny, ImVec2(hx, pos.y + S(40.0f)), C(p.textDim, alpha), T("sp.artist"));
    TextFit(d, g_fonts.hero, ImVec2(hx, pos.y + S(56.0f)), C(p.text, alpha), it.name.c_str(), width - cover - S(40.0f));
    if (followers > 0) {
        char f[64];
        std::snprintf(f, sizeof(f), T("sp.followers"), followers);
        Text(d, g_fonts.regular, ImVec2(hx, pos.y + S(64.0f) + g_fonts.hero->FontSize), C(p.textDim, alpha), f);
    }
    const float by = pos.y + cover - S(26.0f);
    if (PlayButton(d, "##sp_ar_play", ImVec2(hx + S(26.0f), by), S(26.0f), alpha)) PlayItem(it);
    if (IconToggle(d, "##sp_ar_radio", ImVec2(hx + S(82.0f), by), icons::kSignal, S(20.0f), false)) StartRadio(it);
    if (IconToggle(d, "##sp_ar_more", ImVec2(hx + S(126.0f), by), icons::kEllipsis, S(20.0f), false)) OpenMenu(it);
    float y = pos.y + cover + S(30.0f);
    // popular
    const std::vector<Item>* top = nullptr;
    {
        std::shared_ptr<const nlohmann::json> tj;
        luma::launcher::spotifyengine::Ask("artist_top " + it.id, tj);
        static std::map<std::string, std::vector<Item>> tops;
        if (tj) {
            auto& v = tops[it.id];
            if (v.empty()) v = ParseList(*tj, "items");
            top = &v;
        }
    }
    if (top != nullptr && !top->empty()) {
        Text(d, g_fonts.title, ImVec2(pos.x, y), C(p.text, alpha), T("sp.popular"));
        y += g_fonts.title->FontSize + S(12.0f);
        for (std::size_t i = 0; i < top->size() && i < 10; ++i) {
            char rid[32];
            std::snprintf(rid, sizeof(rid), "##sp_top%zu", i);
            TrackRow(d, rid, (*top)[i], static_cast<int>(i) + 1, ImVec2(pos.x, y), width, alpha, width > S(700.0f), true,
                     [top, i]() { PlayUris(*top, i); });
            y += S(54.0f);
        }
        y += S(24.0f);
    }
    // discography
    const std::vector<Item>* albums = nullptr;
    const std::string albumsPath = "/v1/artists/" + it.id + "/albums?include_groups=album,single&limit=30";
    if (Items(albumsPath, albums) == 1 && albums != nullptr) {
        y += Row(d, "##r_disco", T("sp.discography"), *albums, ImVec2(pos.x, y), width, alpha, albumsPath, "items");
    }
    return y - pos.y;
}

float AllPage(ImDrawList* d, Page& pg, ImVec2 pos, float width, float alpha) {
    const Palette& p = P();
    Text(d, g_fonts.hero, pos, C(p.text, alpha), pg.title.c_str());
    const float y = pos.y + g_fonts.hero->FontSize + S(20.0f);
    const std::vector<Item>* list = nullptr;
    if (Items(pg.path, list, pg.key.c_str()) != 1 || list == nullptr) {
        Spinner(d, ImVec2(pos.x + width * 0.5f, y + S(60.0f)), S(16.0f), C(kSpotifyGreen, alpha));
        return S(200.0f);
    }
    if (!list->empty() && (*list)[0].type == kTrack) {  // songs: as a list
        float ty = y;
        for (std::size_t i = 0; i < list->size(); ++i) {
            char rid[32];
            std::snprintf(rid, sizeof(rid), "##sp_all%zu", i);
            TrackRow(d, rid, (*list)[i], static_cast<int>(i) + 1, ImVec2(pos.x, ty), width, alpha, width > S(700.0f), true,
                     [list, i]() { PlayUris(*list, i); });
            ty += S(54.0f);
        }
        return ty - pos.y;
    }
    return y - pos.y + Grid(d, "##g_all", *list, ImVec2(pos.x, y), width, alpha);
}


// THE CARD OF LUMA'S OWN PLAYER (Spotify Soloist, see spotify_engine.h): the
// three steps the first time (your key, the download, pairing), then "Listen
// in Luma". Returns its height.
float LumaPlayerCard(ImDrawList* d, float x, float y, float w, float alpha) {
    namespace eng = luma::launcher::spotifyengine;
    const Palette& p = P();
    const eng::State es = eng::GetState();
    const float pad = S(14.0f);
    float cy = y + pad;
    const float tw = w - pad * 2.0f;
    icons::Draw(d, icons::kLumaLogo, ImVec2(x + pad + S(10.0f), cy + S(9.0f)), S(20.0f), C(p.text, alpha));
    Text(d, g_fonts.semibold, ImVec2(x + pad + S(28.0f), cy), C(p.text, alpha), T("sp.luma_player"));
    cy += g_fonts.semibold->FontSize + S(4.0f);
    cy += TextWrap(d, g_fonts.tiny, ImVec2(x + pad, cy), C(p.textDim, alpha), T("sp.luma_player_hint"), tw) + S(10.0f);
    float bw = 0.0f;
    auto waiting = [&](const char* key) {
        Spinner(d, ImVec2(x + pad + S(10.0f), cy + S(10.0f)), S(9.0f), C(kSpotifyGreen, alpha));
        cy += TextWrap(d, g_fonts.semibold, ImVec2(x + pad + S(28.0f), cy), C(p.text, alpha), T(key), tw - S(28.0f)) + S(12.0f);
    };
    switch (es) {
        case eng::State::NoProgram:
            cy += TextWrap(d, g_fonts.regular, ImVec2(x + pad, cy), C(p.warning, alpha), T("sp.luma_missing"), tw) + S(10.0f);
            break;
        case eng::State::Login: waiting("sp.luma_login"); break;
        case eng::State::Starting: waiting("sp.luma_starting"); break;
        case eng::State::Off:
        case eng::State::Ready:
        case eng::State::Error: {
            if (es == eng::State::Error) {
                const std::string err = std::string(T("sp.luma_error")) + " " + eng::Error();
                cy += TextWrap(d, g_fonts.tiny, ImVec2(x + pad, cy), C(p.warning, alpha), err.c_str(), tw) + S(8.0f);
            }
            const spotify::Player pl = spotify::GetPlayer();
            if (es == eng::State::Ready && pl.hasDevice && pl.device.id == eng::DeviceId()) {
                Text(d, g_fonts.semibold, ImVec2(x + pad, cy + S(6.0f)), C(kSpotifyGreen, alpha), T("spotify.playing_here"));
            } else if (SmallPill(d, "##lp_listen", ImVec2(x + pad, cy), icons::kPlayFilled, T("sp.luma_listen"), true, &bw)) {
                WithLumaDevice([](const std::string& id) { sp::ContinueOn(id); });
            }
            cy += S(46.0f);
            break;
        }
    }
    if (es != eng::State::NoProgram && es != eng::State::Login) {
        const char* out = T("sp.luma_sign_out");
        const ImVec2 fs = TextSize(g_fonts.tiny, out);
        const ImVec2 fa(x + pad, cy - S(2.0f)), fb(fa.x + fs.x, fa.y + fs.y);
        float hover = 0.0f;
        if (Hit("##lp_signout", fa, fb, &hover)) eng::SignOut();
        Text(d, g_fonts.tiny, fa, C(Mix(p.textDim, p.text, hover), alpha), out);
        cy += fs.y + S(6.0f);
    }
    const float h = cy - y + S(6.0f);
    d->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), C(kSpotifyGreen, 0.35f * alpha), S(14.0f));
    return h;
}

// --- The drawer (right): queue, devices, lyrics ------------------------------------------------------

int g_drawer = -1;        // -1 closed, 0 queue, 1 devices, 2 lyrics
int g_drawerShown = 0;    // the tab it shows (kept while it closes)
float g_drawerAnim = 0.0f;

void Drawer(ImDrawList* d, ImVec2 min, ImVec2 max, float alpha) {
    const Palette& p = P();
    const float e = EaseOutCubic(g_drawerAnim);
    const float w = max.x - min.x;
    const ImVec2 a(min.x + (1.0f - e) * (w + S(30.0f)), min.y), b(a.x + w, max.y);
    alpha *= e;
    if (alpha < 0.01f) return;
    d->AddRectFilled(a, b, C(Mix(p.card, ImVec4(0, 0, 0, 1), 0.2f), 0.55f * alpha), S(18.0f));
    d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), 0.12f * alpha), S(18.0f));
    const char* tabs[3] = {T("spotify.up_next"), T("spotify.devices"), T("nav.lyrics")};
    int tab = g_drawerShown;
    const float tw = SegmentedWidth(tabs, 3);
    if (Segmented("##sp_drawer_tabs", ImVec2(a.x + (w - tw) * 0.5f, a.y + S(12.0f)), tabs, 3, &tab)) g_drawer = g_drawerShown = tab;
    const ImVec2 cmin(a.x + S(12.0f), a.y + S(64.0f)), cmax(b.x - S(12.0f), b.y - S(12.0f));
    const ImVec2 savedMin = g_viewMin, savedMax = g_viewMax;
    g_viewMin = cmin;
    g_viewMax = cmax;
    d->PushClipRect(cmin, cmax, true);
    if (g_drawerShown == 0) {
        // what plays, then what comes
        const std::shared_ptr<const nlohmann::json> q = Json("/v1/me/player/queue", nullptr, 4);
        float y = cmin.y;
        if (q && q->is_object()) {
            std::vector<Item> now;
            if (q->contains("currently_playing") && (*q)["currently_playing"].is_object()) {
                Item cur;
                if (Parse((*q)["currently_playing"], cur)) now.push_back(cur);
            }
            static std::vector<Item> list;
            static std::shared_ptr<const nlohmann::json> from;
            if (from != q) {
                from = q;
                list = ParseList(*q, "queue");
            }
            if (!now.empty()) {
                Text(d, g_fonts.tiny, ImVec2(cmin.x + S(4.0f), y), C(p.textDim, alpha), T("music.now_playing"));
                y += g_fonts.tiny->FontSize + S(6.0f);
                TrackRow(d, "##sp_q_now", now[0], 0, ImVec2(cmin.x, y), cmax.x - cmin.x, alpha, false, true, []() {});
                y += S(64.0f);
            }
            Text(d, g_fonts.tiny, ImVec2(cmin.x + S(4.0f), y), C(p.textDim, alpha), T("spotify.up_next"));
            y += g_fonts.tiny->FontSize + S(6.0f);
            for (std::size_t i = 0; i < list.size() && y < cmax.y; ++i) {
                char rid[32];
                std::snprintf(rid, sizeof(rid), "##sp_q%zu", i);
                // (clicking a song of the queue: skip to it)
                TrackRow(d, rid, list[i], static_cast<int>(i) + 1, ImVec2(cmin.x, y), cmax.x - cmin.x, alpha, false, true, [i]() {
                    for (std::size_t k = 0; k <= i; ++k) spotify::PlayerNext();
                });
                y += S(54.0f);
            }
            if (list.empty()) Text(d, g_fonts.tiny, ImVec2(cmin.x + S(4.0f), y), C(p.textDim, alpha), T("spotify.queue_empty"));
        } else {
            Spinner(d, ImVec2((cmin.x + cmax.x) * 0.5f, cmin.y + S(40.0f)), S(14.0f), C(kSpotifyGreen, alpha));
        }
    } else if (g_drawerShown == 1) {
        // LUMA'S OWN PLAYER (Spotify Soloist): its 3 steps, then "Listen in Luma"
        float y0 = cmin.y + LumaPlayerCard(d, cmin.x, cmin.y, cmax.x - cmin.x, alpha) + S(16.0f);
        std::uint32_t version = g_deviceListVersion;
        if (spotify::Devices(g_deviceListVersion, g_deviceList, version)) g_deviceListVersion = version;
        static double asked = -10.0;
        if (ImGui::GetTime() - asked > 5.0) {
            spotify::RequestDevices();
            asked = ImGui::GetTime();
        }
        float y = y0;
        if (g_deviceList.empty()) Text(d, g_fonts.tiny, ImVec2(cmin.x + S(4.0f), y), C(p.textDim, alpha), T("spotify.no_devices"));
        for (std::size_t i = 0; i < g_deviceList.size(); ++i) {
            const spotify::Device& dv = g_deviceList[i];
            const ImVec2 ra(cmin.x, y), rb(cmax.x, y + S(56.0f));
            char id[32];
            std::snprintf(id, sizeof(id), "##sp_dev%zu", i);
            float hover = 0.0f;
            if (Hit(id, ra, rb, &hover) && !dv.active) {
                if (dv.name == luma::launcher::spotifyengine::kDeviceName) {
                    ContinueOn(dv.id);
                } else {
                    spotify::PlayerTransfer(dv.id);
                }
            }
            d->AddRectFilled(ra, ImVec2(rb.x, rb.y - S(6.0f)), C(dv.active ? kSpotifyGreen : p.text, (dv.active ? 0.14f : 0.04f) + 0.06f * hover), S(12.0f));
            const ImVec4 col = dv.active ? kSpotifyGreen : p.text;
            icons::Draw(d, DeviceIcon(dv.type), ImVec2(ra.x + S(24.0f), ra.y + S(25.0f)), S(18.0f), C(col, alpha), S(1.6f));
            TextFit(d, g_fonts.semibold, ImVec2(ra.x + S(48.0f), ra.y + S(7.0f)), C(col, alpha), dv.name.c_str(), rb.x - ra.x - S(60.0f));
            Text(d, g_fonts.tiny, ImVec2(ra.x + S(48.0f), ra.y + S(9.0f) + g_fonts.semibold->FontSize), C(p.textDim, alpha),
                 dv.active ? T("spotify.playing_here") : dv.type.c_str());
            y += S(60.0f);
        }
        // your account
        const spotify::Status st = spotify::GetStatus();
        y += S(16.0f);
        std::string who = st.user + (st.premium == 1 ? "  ·  Premium" : (st.premium == 0 ? "  ·  Free" : ""));
        TextFit(d, g_fonts.semibold, ImVec2(cmin.x + S(4.0f), y), C(p.text, alpha), who.c_str(), cmax.x - cmin.x);
        y += g_fonts.semibold->FontSize + S(10.0f);
        float bw = 0.0f;
        if (SmallPill(d, "##sp_dr_disconnect", ImVec2(cmin.x, y), nullptr, T("spotify.disconnect"), false, &bw)) spotify::Disconnect();
    } else {
        // the lyrics (the same as Home's lyrics view, when the song plays on this PC)
        const media::Track t = media::Current();
        DrawLyricsLines(d, cmin, cmax, t, alpha);
    }
    d->PopClipRect();
    g_viewMin = savedMin;
    g_viewMax = savedMax;
}

// --- Search (a layer over everything; Ctrl+K) -----------------------------------------------------

bool g_searchOpen = false;
float g_searchAnim = 0.0f;
char g_searchText[128] = "";
std::string g_searchQuery;   // what was asked (after a short pause in the typing)
double g_searchTypedAt = 0.0;
int g_searchFilter = 0;      // 0 all, 1 songs, 2 artists, 3 albums, 4 playlists
bool g_searchFocus = false;
float g_searchScroll = 0.0f, g_searchTarget = 0.0f, g_searchH = 0.0f;

void OpenSearch() {
    g_searchOpen = true;
    g_searchFocus = true;
}

// With the box empty: your last searches and what you opened. Returns its height.
float DrawSearchHistory(ImDrawList* d, float x0, float y, float w) {
    const Palette& p = P();
    const float y0 = y;
    if (g_recentQueries.empty() && g_recentPicks.empty()) {
        Text(d, g_fonts.semibold, ImVec2(x0, y + S(10.0f)), C(p.textDim), T("sp.search_empty"));
        return S(40.0f);
    }
    // the header, with "Clear"
    auto header = [&](const char* title, const char* id, bool queries) {
        Text(d, g_fonts.title, ImVec2(x0, y), C(p.text), title);
        const char* clear = T("sp.clear_history");
        const ImVec2 cs = TextSize(g_fonts.semibold, clear);
        const ImVec2 a(x0 + w - cs.x - S(24.0f), y + S(2.0f)), b(x0 + w, y + S(2.0f) + cs.y + S(10.0f));
        float hover = 0.0f;
        if (Hit(id, a, b, &hover)) {
            if (queries) {
                g_recentQueries.clear();
            } else {
                g_recentPicks.clear();
            }
            SaveSearchHistory();
        }
        d->AddRectFilled(a, b, C(p.text, 0.08f * hover), (b.y - a.y) * 0.5f);
        Text(d, g_fonts.semibold, ImVec2(a.x + S(12.0f), a.y + S(5.0f)), C(p.textDim, 0.8f + 0.2f * hover), clear);
        y += g_fonts.title->FontSize + S(16.0f);
    };
    // your last searches: pills (click = search it again)
    if (!g_recentQueries.empty()) {
        header(T("sp.recent_searches"), "##sh_clear_q", true);
        const float h = S(38.0f);
        float x = x0;
        for (std::size_t i = 0; i < g_recentQueries.size(); ++i) {
            const std::string& q = g_recentQueries[i];
            const float pw = TextSize(g_fonts.semibold, q.c_str()).x + S(54.0f);
            if (x + pw > x0 + w && x > x0) {
                x = x0;
                y += h + S(10.0f);
            }
            const ImVec2 a(x, y), b(x + pw, y + h);
            char id[32];
            std::snprintf(id, sizeof(id), "##sh_q%zu", i);
            float hover = 0.0f;
            if (Hit(id, a, b, &hover)) {
                std::snprintf(g_searchText, sizeof(g_searchText), "%s", q.c_str());
                g_searchQuery = q;
                g_searchTarget = 0.0f;
                g_searchFocus = true;
            }
            d->AddRectFilled(a, b, C(p.text, 0.07f + 0.07f * hover), h * 0.5f);
            d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), 0.10f), h * 0.5f);
            icons::Draw(d, icons::kClock, ImVec2(a.x + S(20.0f), (a.y + b.y) * 0.5f), S(15.0f), C(p.textDim), S(1.8f));
            Text(d, g_fonts.semibold, ImVec2(a.x + S(36.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f), C(p.text), q.c_str());
            x += pw + S(10.0f);
        }
        y += h + S(30.0f);
    }
    // what you opened or played from the search: tiles (click = again)
    if (!g_recentPicks.empty()) {
        header(T("sp.recent_picks"), "##sh_clear_p", false);
        const float tw = S(150.0f), gap = S(22.0f);
        const int perRow = std::max(1, static_cast<int>((w + gap) / (tw + gap)));
        const float th = tw + g_fonts.semibold->FontSize + g_fonts.tiny->FontSize + S(14.0f);
        for (std::size_t i = 0; i < g_recentPicks.size(); ++i) {
            const int col = static_cast<int>(i) % perRow, row = static_cast<int>(i) / perRow;
            const ImVec2 pos(x0 + col * (tw + gap), y + row * (th + S(18.0f)));
            char id[32];
            std::snprintf(id, sizeof(id), "##sh_p%zu", i);
            const Item it = g_recentPicks[i];  // (a copy: a click moves it to the front)
            const std::vector<Item> one{it};   // (a song plays in its album, as everywhere)
            Tile(d, id, it, pos, tw, 1.0f, &one, 0);
        }
        const int rows = (static_cast<int>(g_recentPicks.size()) + perRow - 1) / perRow;
        y += rows * (th + S(18.0f));
    }
    return y - y0;
}

void SearchLayer(ImDrawList* d, ImVec2 display) {
    g_searchAnim += ((g_searchOpen ? 1.0f : 0.0f) - g_searchAnim) * std::min(1.0f, Dt() * 14.0f);
    if (g_searchAnim < 0.01f) return;
    LoadSearchHistory();
    const Palette& p = P();
    const float e = EaseOutCubic(g_searchAnim);
    const int start = d->VtxBuffer.Size;
    // (a soft shade over the album's colors: the page behind is faded away)
    d->AddRectFilled(ImVec2(0, 0), display, C(Mix(p.background, ImVec4(0, 0, 0, 1), 0.4f), 0.35f));
    const float w = std::min(display.x - S(160.0f), S(1100.0f));
    const float x0 = (display.x - w) * 0.5f;
    // the box: a glass pill, centered, with a soft light of the album's color
    const float bw = std::min(w, S(720.0f));
    const ImVec2 ba(x0 + (w - bw) * 0.5f, S(66.0f)), bb(ba.x + bw, S(66.0f) + S(50.0f));
    const float bcy = (ba.y + bb.y) * 0.5f;
    SoftLightOval(d, ImVec2((ba.x + bb.x) * 0.5f, bcy), bw * 0.55f, S(46.0f), p.accent, 0.35f);
    d->AddRectFilled(ba, bb, C(Mix(p.card, ImVec4(1, 1, 1, 1), 0.05f), 0.92f), S(25.0f));
    d->AddRect(ba, bb, C(ImVec4(1, 1, 1, 1), 0.14f), S(25.0f));
    icons::Draw(d, icons::kSearch, ImVec2(ba.x + S(26.0f), bcy), S(17.0f), C(p.text, 0.8f), S(2.0f));
    // the X: clears what you typed (Esc or a click outside close the search)
    if (g_searchText[0] != '\0') {
        const ImVec2 xc(bb.x - S(25.0f), bcy);
        float hover = 0.0f;
        if (Hit("##sp_search_x", ImVec2(xc.x - S(16.0f), xc.y - S(16.0f)), ImVec2(xc.x + S(16.0f), xc.y + S(16.0f)), &hover)) {
            g_searchText[0] = '\0';
            g_searchQuery.clear();
            g_searchTarget = 0.0f;
            g_searchFocus = true;
        }
        d->AddCircleFilled(xc, S(15.0f), C(p.text, 0.06f + 0.10f * hover), 32);
        icons::Draw(d, icons::kClose, xc, S(14.0f), C(p.text, 0.7f + 0.3f * hover), S(2.0f));
    }
    ImGui::SetCursorScreenPos(ImVec2(ba.x + S(48.0f), bcy - g_fonts.line->FontSize * 0.5f - S(3.0f)));
    ImGui::PushFont(g_fonts.line);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.textDim);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(3.0f)));
    ImGui::SetNextItemWidth(bw - S(100.0f));
    if (g_searchFocus) {
        ImGui::SetKeyboardFocusHere();
        g_searchFocus = false;
    }
    if (ImGui::InputTextWithHint("##sp_search", T("sp.search_hint"), g_searchText, sizeof(g_searchText))) g_searchTypedAt = ImGui::GetTime();
    const bool enter = ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    if (ImGui::GetTime() - g_searchTypedAt > 0.35 && g_searchQuery != g_searchText) {
        g_searchQuery = g_searchText;
        g_searchTarget = 0.0f;
    }
    // the filters
    const char* filters[5] = {T("sp.all"), T("sp.songs"), T("sp.artists"), T("sp.albums"), T("sp.playlists")};
    Segmented("##sp_search_filter", ImVec2(x0 + (w - SegmentedWidth(filters, 5)) * 0.5f, bb.y + S(16.0f)), filters, 5, &g_searchFilter);
    // the results
    const ImVec2 rmin(x0, bb.y + S(72.0f)), rmax(x0 + w, display.y - S(20.0f));
    const ImVec2 savedMin = g_viewMin, savedMax = g_viewMax;
    g_viewMin = rmin;
    g_viewMax = rmax;
    d->PushClipRect(rmin, rmax, true);
    float y = rmin.y - g_searchScroll;
    std::string q;
    for (char c : g_searchQuery) q += c;
    while (!q.empty() && q.back() == ' ') q.pop_back();
    g_recordPicks = true;
    g_hasPick = false;
    if (q.empty()) {
        g_searchH = DrawSearchHistory(d, x0, y, w);
    } else {
        const std::string path = "/v1/search?q=" + UrlEncode(q) + "&type=track,artist,album,playlist&limit=10";
        int state = 0;
        const auto j = Json(path, &state, 120);
        if (!j) {
            if (state == 2) {
                Text(d, g_fonts.semibold, ImVec2(x0, y + S(10.0f)), C(p.warning), T("sp.search_failed"));
            } else {
                Spinner(d, ImVec2(x0 + w * 0.5f, y + S(50.0f)), S(16.0f), C(kSpotifyGreen));
            }
        } else {
            static std::shared_ptr<const nlohmann::json> from;
            static std::vector<Item> tracks, artists, albums, lists;
            if (from != j) {
                from = j;
                tracks = ParseList(*j, "tracks");
                artists = ParseList(*j, "artists");
                albums = ParseList(*j, "albums");
                lists = ParseList(*j, "playlists");
            }
            const float y0 = y;
            if (g_searchFilter == 0) {
                // the best match (the first artist, big) and the songs
                if (!artists.empty()) {
                    const float cw = std::min(S(300.0f), w * 0.3f);
                    Text(d, g_fonts.title, ImVec2(x0, y), C(p.text), T("sp.top_result"));
                    Tile(d, "##s_best", artists[0], ImVec2(x0, y + g_fonts.title->FontSize + S(14.0f)), cw * 0.8f, 1.0f);
                    float ty = y;
                    Text(d, g_fonts.title, ImVec2(x0 + cw + S(24.0f), ty), C(p.text), T("sp.songs"));
                    ty += g_fonts.title->FontSize + S(14.0f);
                    for (std::size_t i = 0; i < tracks.size() && i < 5; ++i) {
                        char rid[32];
                        std::snprintf(rid, sizeof(rid), "##s_t%zu", i);
                        TrackRow(d, rid, tracks[i], 0, ImVec2(x0 + cw + S(24.0f), ty), w - cw - S(24.0f), 1.0f, false, true,
                                 [i]() { PlayUris(tracks, i); });
                        ty += S(54.0f);
                    }
                    y = std::max(ty, y + g_fonts.title->FontSize + S(14.0f) + cw * 0.8f + S(60.0f)) + S(20.0f);
                }
                y += Row(d, "##s_ar", T("sp.artists"), artists, ImVec2(x0, y), w, 1.0f, {}, nullptr);
                y += Row(d, "##s_al", T("sp.albums"), albums, ImVec2(x0, y), w, 1.0f, {}, nullptr);
                y += Row(d, "##s_pl", T("sp.playlists"), lists, ImVec2(x0, y), w, 1.0f, {}, nullptr);
            } else if (g_searchFilter == 1) {
                for (std::size_t i = 0; i < tracks.size(); ++i) {
                    char rid[32];
                    std::snprintf(rid, sizeof(rid), "##s_t%zu", i);
                    TrackRow(d, rid, tracks[i], static_cast<int>(i) + 1, ImVec2(x0, y), w, 1.0f, true, true, [i]() { PlayUris(tracks, i); });
                    y += S(54.0f);
                }
            } else {
                const std::vector<Item>& list = g_searchFilter == 2 ? artists : (g_searchFilter == 3 ? albums : lists);
                y += Grid(d, "##s_grid", list, ImVec2(x0, y), w, 1.0f);
            }
            g_searchH = y - y0;
        }
    }
    d->PopClipRect();
    g_recordPicks = false;
    // (a result you opened or played, and what you had searched, are remembered)
    if (g_hasPick) {
        RememberPick(g_pick);
        RememberQuery(q);
        g_hasPick = false;
    } else if (enter) {
        RememberQuery(q);
    }
    g_viewMin = savedMin;
    g_viewMax = savedMax;
    // scrolling the results
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.y > rmin.y && mouse.y < rmax.y) g_searchTarget -= ImGui::GetIO().MouseWheel * S(90.0f);
    g_searchTarget = std::clamp(g_searchTarget, 0.0f, std::max(0.0f, g_searchH - (rmax.y - rmin.y) + S(40.0f)));
    g_searchScroll += (g_searchTarget - g_searchScroll) * std::min(1.0f, Dt() * 14.0f);
    // opening something from the results closes the search
    static std::size_t depth = 1;
    if (g_stack.size() != depth) {
        depth = g_stack.size();
        if (g_searchOpen && g_stack.size() > 1) g_searchOpen = false;
    }
    if (g_searchOpen && ImGui::IsKeyPressed(ImGuiKey_Escape) && !g_menu.open) g_searchOpen = false;
    // a click on nothing (outside the box and the results) closes it
    if (g_searchOpen && g_searchAnim > 0.9f && !g_menu.open && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        g_searchOpen = false;
    }
    AnimateSince(d, start, e, ImVec2(0.0f, (1.0f - e) * S(-12.0f)));
}

}  // namespace sp

namespace sp {

// --- The top: what plays, search, the drawer's buttons ---------------------------------------------

void TopBar(ImDrawList* d, ImVec2 min, float width, const spotify::Status& st) {
    const Palette& p = P();
    const spotify::Player pl = spotify::GetPlayer();
    const float h = S(84.0f);
    const float cy = min.y + h * 0.5f;
    float x = min.x;
    if (pl.valid && pl.hasDevice && !pl.title.empty()) {
        const float art = S(76.0f);
        const textures::Texture* tex = pl.artPath.empty() ? nullptr : textures::GetAsync(pl.artPath);
        const ImVec2 a(x, cy - art * 0.5f), b(x + art, cy + art * 0.5f);
        SoftLight(d, ImVec2((a.x + b.x) * 0.5f, cy), art * 0.9f, p.accent, 0.4f + 0.3f * g_bassSmooth);
        if (tex != nullptr) {
            d->AddImageRounded(tex->id, a, b, ImVec2(0, 0), ImVec2(1, 1), C(ImVec4(1, 1, 1, 1)), S(10.0f));
        } else {
            d->AddRectFilled(a, b, C(p.card), S(10.0f));
        }
        x += art + S(16.0f);
        const float tw = std::min(S(300.0f), width * 0.25f);
        TextFit(d, g_fonts.title, ImVec2(x, cy - S(30.0f)), C(p.text), pl.title.c_str(), tw);
        TextFit(d, g_fonts.regular, ImVec2(x, cy - S(2.0f)), C(p.textDim), pl.artist.c_str(), tw);
        // the progress, thin, under the name
        {
            media::Track fake;
            fake.present = true;
            fake.position = spotify::PositionSeconds(pl);
            fake.duration = pl.durationMs / 1000.0;
            fake.canSeek = true;
            ThinSeek(d, "##sp_top_seek", x, x + tw, cy + S(28.0f), fake, 1.0f, SpotifySeekTo);
        }
        x += tw + S(30.0f);
        // the controls
        const float bx = x + S(18.0f);
        if (IconToggle(d, "##sp_t_shuffle", ImVec2(bx, cy), icons::kShuffle, S(18.0f), pl.shuffle)) spotify::PlayerShuffle(!pl.shuffle);
        if (RoundButton(d, "##sp_t_prev", ImVec2(bx + S(48.0f), cy), S(20.0f), icons::kSkipBackFilled, S(15.0f), false)) spotify::PlayerPrevious();
        if (RoundButton(d, "##sp_t_play", ImVec2(bx + S(104.0f), cy), S(27.0f), pl.playing ? icons::kPauseFilled : icons::kPlayFilled, S(19.0f), true)) {
            spotify::PlayerPlay(!pl.playing);
        }
        if (RoundButton(d, "##sp_t_next", ImVec2(bx + S(160.0f), cy), S(20.0f), icons::kSkipForwardFilled, S(15.0f), false)) spotify::PlayerNext();
        {
            const int nextMode = pl.repeat == 0 ? 2 : (pl.repeat == 2 ? 1 : 0);
            if (IconToggle(d, "##sp_t_repeat", ImVec2(bx + S(208.0f), cy), pl.repeat == 1 ? icons::kRepeat1 : icons::kRepeat, S(18.0f), pl.repeat != 0)) {
                spotify::PlayerRepeat(nextMode);
            }
        }
        if (IconToggle(d, "##sp_t_heart", ImVec2(bx + S(252.0f), cy), pl.liked ? icons::kHeartFilled : icons::kHeart, S(18.0f), pl.liked,
                       pl.likedKnown)) {
            spotify::PlayerLike(!pl.liked);
        }
        x = bx + S(290.0f);
        SpotifyVolumeBar(d, ImVec2(x, cy), std::clamp(width - (x - min.x) - S(420.0f), S(50.0f), S(110.0f)), pl, 1.0f);
    } else if (pl.valid) {
        namespace eng = luma::launcher::spotifyengine;
        const bool starting = eng::GetState() == eng::State::Starting || g_whenLumaReady != nullptr;
        Text(d, g_fonts.semibold, ImVec2(x, cy - S(24.0f)), C(p.text), T("spotify.nothing_title"));
        if (starting) {
            Spinner(d, ImVec2(x + S(10.0f), cy + S(16.0f)), S(8.0f), C(kSpotifyGreen));
            Text(d, g_fonts.tiny, ImVec2(x + S(26.0f), cy + S(16.0f) - g_fonts.tiny->FontSize * 0.5f), C(p.textDim), T("sp.luma_starting"));
        } else {
            float bw = 0.0f;
            if (SmallPill(d, "##sp_t_luma", ImVec2(x, cy), icons::kPlayFilled, T("sp.luma_listen"), true, &bw)) {
                WithLumaDevice([](const std::string& id) { sp::ContinueOn(id); });
            }
        }
    }
    // right: search, the drawer's three buttons
    float rx = min.x + width;
    const char* tips[3] = {"##sp_d_queue", "##sp_d_devices", "##sp_d_lyrics"};
    const char* dicons[3] = {icons::kList, icons::kAppWindow, icons::kMicVocal};
    for (int i = 2; i >= 0; --i) {
        rx -= S(40.0f);
        if (IconToggle(d, tips[i], ImVec2(rx + S(18.0f), cy), dicons[i], S(18.0f), g_drawer == i)) {
            g_drawer = g_drawer == i ? -1 : i;
            if (g_drawer >= 0) g_drawerShown = g_drawer;
        }
    }
    rx -= S(12.0f);
    {
        const char* label = T("sp.search");
        const ImVec2 ls = TextSize(g_fonts.semibold, label);
        const float sw = ls.x + S(88.0f);
        const ImVec2 a(rx - sw, cy - S(19.0f)), b(rx, cy + S(19.0f));
        float hover = 0.0f;
        if (Hit("##sp_t_search", a, b, &hover)) OpenSearch();
        d->AddRectFilled(a, b, C(p.text, 0.08f + 0.06f * hover), S(19.0f));
        d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), 0.12f), S(19.0f));
        icons::Draw(d, icons::kSearch, ImVec2(a.x + S(20.0f), cy), S(16.0f), C(p.text), S(2.0f));
        Text(d, g_fonts.semibold, ImVec2(a.x + S(38.0f), cy - ls.y * 0.5f), C(p.text), label);
        Text(d, g_fonts.tiny, ImVec2(b.x - S(44.0f), cy - g_fonts.tiny->FontSize * 0.5f), C(p.textDim), "Ctrl+K");
    }
    (void)st;
}

// The page (with its scroll): the tab, or what was opened.
void Content(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Page& pg = g_stack.back();
    pg.in = std::min(1.0f, pg.in + Dt() * 5.0f);
    const float e = EaseOutCubic(pg.in);
    // the tabs (or the arrow back)
    float y = min.y;
    if (g_stack.size() > 1) {
        float hover = 0.0f;
        const ImVec2 a(min.x, y), b(min.x + S(40.0f), y + S(40.0f));
        if (Hit("##sp_back", a, b, &hover) || ImGui::IsMouseClicked(ImGuiMouseButton(3))) g_stack.pop_back();
        d->AddCircleFilled(ImVec2(min.x + S(20.0f), y + S(20.0f)), S(18.0f), C(p.text, 0.08f + 0.08f * hover), 32);
        icons::Draw(d, icons::kBack, ImVec2(min.x + S(20.0f), y + S(20.0f)), S(18.0f), C(p.text), S(2.0f));
        if (g_stack.empty()) g_stack.push_back(Page{});
    }
    {
        const char* tabs[2] = {T("nav.home"), T("sp.library")};
        int tab = g_stack.front().kind == kTabLibrary ? 1 : 0;
        const float tx = min.x + (g_stack.size() > 1 ? S(54.0f) : 0.0f);
        if (Segmented("##sp_tabs", ImVec2(tx, y + S(1.0f)), tabs, 2, &tab) || (g_stack.size() > 1 && false)) {
            g_stack.clear();
            Page root;
            root.kind = tab == 1 ? kTabLibrary : kTabHome;
            g_stack.push_back(root);
        }
    }
    y += S(58.0f);
    Page& cur = g_stack.back();
    const ImVec2 vmin(min.x, y), vmax(max.x, max.y);
    g_viewMin = vmin;
    g_viewMax = vmax;
    d->PushClipRect(ImVec2(vmin.x - S(12.0f), vmin.y), ImVec2(vmax.x + S(12.0f), vmax.y), true);
    const int start = d->VtxBuffer.Size;
    const ImVec2 pos(vmin.x, vmin.y + S(8.0f) - cur.scroll);
    const float width = vmax.x - vmin.x;
    float h = 0.0f;
    switch (cur.kind) {
        case kTabHome: h = HomeTab(d, pos, width, 1.0f); break;
        case kTabLibrary: h = LibraryTab(d, pos, width, 1.0f); break;
        case kPageArtist: h = ArtistPage(d, cur, pos, width, 1.0f); break;
        case kPageAll: h = AllPage(d, cur, pos, width, 1.0f); break;
        default: h = SongsPage(d, cur, pos, width, 1.0f); break;
    }
    AnimateSince(d, start, e, ImVec2(0.0f, (1.0f - e) * S(18.0f)));
    d->PopClipRect();
    // (soft edges: the page fades out at the top and the bottom)
    cur.contentH = h + S(40.0f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!g_searchOpen && !g_menu.open && mouse.x > vmin.x && mouse.x < vmax.x && mouse.y > vmin.y && mouse.y < vmax.y) {
        cur.target -= ImGui::GetIO().MouseWheel * S(110.0f);
    }
    cur.target = std::clamp(cur.target, 0.0f, std::max(0.0f, cur.contentH - (vmax.y - vmin.y)));
    cur.scroll += (cur.target - cur.scroll) * std::min(1.0f, Dt() * 14.0f);
    // a thin bar showing where you are
    if (cur.contentH > vmax.y - vmin.y + S(10.0f)) {
        const float viewH = vmax.y - vmin.y;
        const float barH = std::max(S(40.0f), viewH * viewH / cur.contentH);
        const float barY = vmin.y + (viewH - barH) * (cur.scroll / std::max(1.0f, cur.contentH - viewH));
        d->AddRectFilled(ImVec2(vmax.x + S(6.0f), barY), ImVec2(vmax.x + S(9.0f), barY + barH), C(p.text, 0.18f), S(2.0f));
    }
}

}  // namespace sp

bool g_overlaysWanted = false;

bool SpotifySearchOpen() {
    return sp::g_searchOpen;
}

// THE WHOLE SECTION (when connected with the new permissions).
void DrawSpotifySection(ImDrawList* d, ImVec2 min, ImVec2 max, ImVec2 display, const spotify::Status& st) {
    using namespace sp;
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) OpenSearch();
    LumaPlayerTick();
    const float drawerW = std::clamp((max.x - min.x) * 0.26f, S(300.0f), S(380.0f));
    if (g_drawerAsk >= 0) {
        g_drawer = g_drawerShown = g_drawerAsk;
        g_drawerAsk = -1;
    }
    g_drawerAnim += ((g_drawer >= 0 ? 1.0f : 0.0f) - g_drawerAnim) * std::min(1.0f, Dt() * 12.0f);
    const float de = EaseOutCubic(g_drawerAnim);
    // (while searching, the page behind fades away and can't be
    // touched: the results stand alone on the album's colors)
    const bool savedBlock = g_blockInput;
    if (g_searchOpen) g_blockInput = true;
    const int pageStart = d->VtxBuffer.Size;
    TopBar(d, min, max.x - min.x, st);
    const float top = min.y + S(100.0f);
    const float contentRight = max.x - (drawerW + S(24.0f)) * de;
    Content(d, ImVec2(min.x, top), ImVec2(contentRight - S(14.0f), max.y));
    Drawer(d, ImVec2(max.x - drawerW, top), ImVec2(max.x, max.y), 1.0f);
    if (g_searchAnim > 0.01f) {
        const float se = EaseOutCubic(g_searchAnim);
        AnimateSince(d, pageStart, 1.0f - se, ImVec2(0.0f, se * S(10.0f)));
    }
    g_blockInput = savedBlock;
    if (const char* err = SpotifyErrorText(spotify::PlayerError())) {
        const ImVec2 es = TextSize(g_fonts.tiny, err);
        const ImVec2 a(min.x + (max.x - min.x - es.x) * 0.5f - S(14.0f), max.y - S(30.0f)), b(a.x + es.x + S(28.0f), max.y - S(4.0f));
        d->AddRectFilled(a, b, C(Mix(P().card, ImVec4(0, 0, 0, 1), 0.3f), 0.9f), S(13.0f));
        Text(d, g_fonts.tiny, ImVec2(a.x + S(14.0f), (a.y + b.y - es.y) * 0.5f), C(P().warning), err);
    }
    RadioTick();
    RadioPlayTick();
    g_overlaysWanted = true;  // (the device list, the search and the options: drawn after the dock, over it)
}

void DrawSpotifyOverlays(ImDrawList* d, ImVec2 display) {
    using namespace sp;
    if (!g_overlaysWanted) return;
    g_overlaysWanted = false;
    DrawDevicesMenu(d);
    SearchLayer(d, display);
    DrawMenu(d, display);
}

void DrawSpotifyPage(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    flowcfg::Refresh();
    SyncSpotifyClientId();
    const spotify::Status st = spotify::GetStatus();
    if (st.state == spotify::State::Connected && !st.outdated) {
        DrawSpotifySection(d, min, max, ImGui::GetIO().DisplaySize, st);
        return;
    }
    const float W = max.x - min.x;
    // Header: the logo, the name, what it is
    DrawSpotifyLogo(d, ImVec2(min.x + S(22.0f), min.y + S(20.0f)), S(40.0f), g_fade);
    Text(d, g_fonts.title, ImVec2(min.x + S(52.0f), min.y + S(2.0f)), C(p.text), "Spotify");
    TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(52.0f), min.y + S(8.0f) + g_fonts.title->FontSize), C(p.textDim), T("spotify.subtitle"),
             std::min(W * 0.6f, S(600.0f)));
    float y = min.y + S(84.0f);
    const float now = static_cast<float>(ImGui::GetTime());

    // --- Connected (with the new permissions): your account, small, and THE PLAYER ----------
    if (st.state == spotify::State::Connected && !st.outdated) {
        float bw = 0.0f;
        const char* off = T("spotify.disconnect");
        const float offW = TextSize(g_fonts.semibold, off).x + S(28.0f);
        if (SmallPill(d, "##sp_disconnect", ImVec2(max.x - offW, min.y + S(4.0f)), nullptr, off, false, &bw)) spotify::Disconnect();
        std::string who = st.user;
        if (st.premium == 1) who += "  ·  Premium";
        if (st.premium == 0) who += "  ·  Free";
        const ImVec2 ws = TextSize(g_fonts.semibold, who.c_str());
        Text(d, g_fonts.semibold, ImVec2(max.x - offW - S(14.0f) - ws.x, min.y + S(4.0f) + (S(34.0f) - ws.y) * 0.5f), C(p.text, 0.85f), who.c_str());
        if (st.premium == 0) {
            TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(4.0f), y - S(10.0f)), C(p.warning), T("spotify.free_note"), std::min(W, S(720.0f)));
            y += S(26.0f);
        }
        DrawSpotifyPlayer(d, ImVec2(min.x, y), max, st);
        DrawDevicesMenu(d);
        return;
    }

    // --- Connected with the old permissions: your account, and "connect again" ---------------
    if (st.state == spotify::State::Connected) {
        const ImVec2 cmin(min.x, y), cmax(min.x + std::min(W, S(720.0f)), y + S(150.0f));
        Card(d, cmin, cmax);
        const ImVec2 av(cmin.x + S(46.0f), cmin.y + S(46.0f));
        d->AddCircleFilled(av, S(24.0f), C(kSpotifyGreen, 0.18f), 40);
        DrawSpotifyLogo(d, av, S(30.0f), g_fade);
        float tx = av.x + S(40.0f);
        const std::string name = st.user.empty() ? std::string(T("spotify.connected")) : st.user;
        Text(d, g_fonts.title, ImVec2(tx, av.y - g_fonts.title->FontSize * 0.55f - S(6.0f)), C(p.text), name.c_str());
        {
            const char* plan = st.premium == 1 ? "Premium" : (st.premium == 0 ? "Free" : "...");
            const ImVec4 pc = st.premium == 1 ? kSpotifyGreen : p.textDim;
            const ImVec2 ps = TextSize(g_fonts.tiny, plan);
            const ImVec2 pa(tx, av.y + S(10.0f)), pb(tx + ps.x + S(16.0f), av.y + S(10.0f) + ps.y + S(6.0f));
            d->AddRectFilled(pa, pb, C(pc, 0.18f), (pb.y - pa.y) * 0.5f);
            Text(d, g_fonts.tiny, ImVec2(pa.x + S(8.0f), pa.y + S(3.0f)), C(pc), plan);
            Text(d, g_fonts.tiny, ImVec2(pb.x + S(10.0f), pa.y + S(3.0f)), C(p.textDim), T("spotify.shared"));
        }
        float bx = cmin.x + S(22.0f);
        const float by = cmax.y - S(52.0f);
        float bw = 0.0f;
        if (st.outdated) {
            if (SmallPill(d, "##sp_reconnect", ImVec2(bx, by), icons::kRotate, T("spotify.reconnect"), true, &bw)) spotify::Connect();
            bx += bw + S(10.0f);
        }
        if (SmallPill(d, "##sp_disconnect", ImVec2(bx, by), nullptr, T("spotify.disconnect"), false, &bw)) spotify::Disconnect();
        y = cmax.y + S(16.0f);
        // what it means / what comes
        const char* note = st.outdated ? T("spotify.outdated") : (st.premium == 0 ? T("spotify.free_note") : T("spotify.next_part"));
        y += TextWrap(d, g_fonts.regular, ImVec2(min.x + S(4.0f), y), C(st.outdated || st.premium == 0 ? p.warning : p.textDim), note,
                      std::min(W, S(720.0f)));
        return;
    }

    // --- Waiting for you in the browser --------------------------------------------------------
    if (st.state == spotify::State::SigningIn) {
        const ImVec2 c(min.x + std::min(W, S(720.0f)) * 0.5f, y + S(70.0f));
        Spinner(d, c, S(18.0f), C(kSpotifyGreen));
        const char* text = T("spotify.waiting");
        const ImVec2 ts = TextSize(g_fonts.semibold, text);
        Text(d, g_fonts.semibold, ImVec2(c.x - ts.x * 0.5f, c.y + S(32.0f)), C(p.text), text);
        float bw = 0.0f;
        const char* cancel = T("spotify.cancel");
        const float cw = TextSize(g_fonts.semibold, cancel).x + S(28.0f);
        if (SmallPill(d, "##sp_cancel", ImVec2(c.x - cw * 0.5f, c.y + S(64.0f)), nullptr, cancel, false, &bw)) spotify::Disconnect();
        return;
    }

    // --- Not connected: the guide, step by step ------------------------------------------------
    const float colW = std::min(W, S(760.0f));
    if (st.state == spotify::State::Error) {
        std::string msg = T("spotify.error");
        if (!st.error.empty()) msg += "  (" + st.error + ")";
        Dot(d, ImVec2(min.x + S(10.0f), y + g_fonts.tiny->FontSize * 0.5f), p.danger);
        TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(22.0f), y), C(p.danger), msg.c_str(), colW - S(22.0f));
        y += g_fonts.tiny->FontSize * 2.0f + S(8.0f);
    }
    Text(d, g_fonts.semibold, ImVec2(min.x + S(4.0f), y), C(p.text), T("spotify.guide_title"));
    y += g_fonts.semibold->FontSize + S(4.0f);
    y += TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(4.0f), y), C(p.textDim), T("spotify.guide_why"), colW) + S(12.0f);
    const bool hasId = LooksLikeClientId(flowcfg::g_state.spotifyClientId);
    auto step = [&](int n, const char* key, float extraH) {
        const ImVec2 c(min.x + S(16.0f), y + S(11.0f));
        const bool done = (n <= 4 && hasId) || (n == 5 && hasId);
        d->AddCircleFilled(c, S(12.0f), C(done ? kSpotifyGreen : p.text, done ? 1.0f : 0.10f), 32);
        if (done) {
            icons::Draw(d, kIconCheck, c, S(12.0f), C(ImVec4(0.02f, 0.02f, 0.02f, 1)), S(2.4f));
        } else {
            char num[4];
            std::snprintf(num, sizeof(num), "%d", n);
            const ImVec2 ns = TextSize(g_fonts.semibold, num);
            Text(d, g_fonts.semibold, ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.5f), C(p.text), num);
        }
        const float th = TextWrap(d, g_fonts.regular, ImVec2(min.x + S(40.0f), y + S(1.0f)), C(p.text), T(key), colW - S(40.0f));
        const float rowY = y + std::max(th, S(22.0f)) + S(8.0f);
        y = rowY + extraH;
        return rowY;
    };
    float bw = 0.0f;
    {
        const float ry = step(1, "spotify.step1", S(42.0f));
        if (SmallPill(d, "##sp_open", ImVec2(min.x + S(40.0f), ry), icons::kAppWindow, T("spotify.open_dashboard"), false, &bw)) {
            ShellExecuteW(nullptr, L"open", L"https://developer.spotify.com/dashboard", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    step(2, "spotify.step2", S(6.0f));
    {
        const float ry = step(3, "spotify.step3", S(42.0f));
        // the address, as code, with "Copy"
        const char* uri = spotify::kRedirectUri;
        const ImVec2 us = TextSize(g_fonts.semibold, uri);
        const ImVec2 a(min.x + S(40.0f), ry), b(a.x + us.x + S(24.0f), ry + S(34.0f));
        d->AddRectFilled(a, b, C(ImVec4(0, 0, 0, 1), 0.30f), S(8.0f));
        Text(d, g_fonts.semibold, ImVec2(a.x + S(12.0f), (a.y + b.y - us.y) * 0.5f), C(kSpotifyGreen), uri);
        static double copiedAt = -10.0;
        const bool copied = ImGui::GetTime() - copiedAt < 1.5;
        if (SmallPill(d, "##sp_copyuri", ImVec2(b.x + S(10.0f), ry), copied ? kIconCheck : icons::kCopy, copied ? T("opt.copied") : T("spotify.copy"),
                      false, &bw)) {
            ImGui::SetClipboardText(uri);
            copiedAt = ImGui::GetTime();
        }
    }
    step(4, "spotify.step4", S(6.0f));
    {
        const float ry = step(5, "spotify.step5", S(46.0f));
        // the Client ID box (edit in place, or paste)
        static char buf[80] = "";
        static bool init = false;
        if (!init || (!ImGui::IsAnyItemActive() && flowcfg::g_state.spotifyClientId != buf && !ImGui::IsItemActive())) {
            static std::string last;
            if (!init || last != flowcfg::g_state.spotifyClientId) {
                std::snprintf(buf, sizeof(buf), "%s", flowcfg::g_state.spotifyClientId.c_str());
                last = flowcfg::g_state.spotifyClientId;
            }
            init = true;
        }
        const float boxW = S(380.0f);
        const ImVec2 a(min.x + S(40.0f), ry), b(a.x + boxW, ry + S(38.0f));
        const std::string cleaned = CleanClientId(buf);
        const bool ok = LooksLikeClientId(cleaned);
        d->AddRectFilled(a, b, C(ImVec4(0, 0, 0, 1), 0.30f), S(10.0f));
        d->AddRect(a, b, C(cleaned.empty() ? ImVec4(1, 1, 1, 0.15f) : (ok ? kSpotifyGreen : p.warning), cleaned.empty() ? 1.0f : 0.8f),
                   S(10.0f), 0, S(1.5f));
        ImGui::SetCursorScreenPos(ImVec2(a.x + S(12.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f - S(3.0f)));
        ImGui::PushFont(g_fonts.semibold);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, p.text);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.textDim);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(3.0f)));
        ImGui::SetNextItemWidth(boxW - S(24.0f));
        const bool edited = ImGui::InputTextWithHint("##sp_clientid", T("spotify.client_id_hint"), buf, sizeof(buf));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
        if (edited) {
            const std::string id = CleanClientId(buf);
            if (id.empty() || LooksLikeClientId(id)) SaveSpotifyClientId(id);
        }
        float x = b.x + S(10.0f);
        if (SmallPill(d, "##sp_paste", ImVec2(x, ry + S(2.0f)), nullptr, T("spotify.paste"), false, &bw)) {
            const char* clip = ImGui::GetClipboardText();
            if (clip != nullptr) {
                std::snprintf(buf, sizeof(buf), "%s", CleanClientId(clip).c_str());
                const std::string id = CleanClientId(buf);
                if (LooksLikeClientId(id)) SaveSpotifyClientId(id);
            }
        }
        if (!cleaned.empty() && !ok) {
            Text(d, g_fonts.tiny, ImVec2(a.x + S(4.0f), b.y + S(4.0f)), C(p.warning), T("spotify.bad_id"));
        }
    }
    // 6. Connect
    {
        const float ry = step(6, "spotify.step6", S(50.0f));
        const char* label = T("spotify.connect");
        if (!hasId) {
            const ImVec2 ts = TextSize(g_fonts.semibold, label);
            const ImVec2 a(min.x + S(40.0f), ry), b(a.x + ts.x + S(64.0f), ry + S(40.0f));
            d->AddRectFilled(a, b, C(p.text, 0.06f), S(20.0f));
            Text(d, g_fonts.semibold, ImVec2(a.x + S(44.0f), (a.y + b.y - ts.y) * 0.5f), C(p.textDim), label);
            DrawSpotifyLogo(d, ImVec2(a.x + S(24.0f), (a.y + b.y) * 0.5f), S(20.0f), 0.4f * g_fade);
        } else {
            const ImVec2 ts = TextSize(g_fonts.semibold, label);
            const ImVec2 a(min.x + S(40.0f), ry), b(a.x + ts.x + S(64.0f), ry + S(40.0f));
            float hover = 0.0f;
            if (Hit("##sp_connect", a, b, &hover)) spotify::Connect();
            const float breathe = 0.5f + 0.5f * std::sin(now * 2.2f);
            SoftLightOval(d, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), (b.x - a.x) * 0.75f, S(40.0f), kSpotifyGreen,
                          (0.35f + 0.25f * breathe + 0.3f * hover) * g_fade);
            d->AddRectFilled(a, b, C(Mix(kSpotifyGreen, ImVec4(1, 1, 1, 1), 0.12f * hover)), S(20.0f));
            d->AddCircleFilled(ImVec2(a.x + S(24.0f), (a.y + b.y) * 0.5f), S(10.0f), C(ImVec4(0, 0, 0, 1)), 24);
            icons::Draw(d, kSpotifyLogo, ImVec2(a.x + S(24.0f), (a.y + b.y) * 0.5f), S(20.0f), C(kSpotifyGreen));
            Text(d, g_fonts.semibold, ImVec2(a.x + S(44.0f), (a.y + b.y - ts.y) * 0.5f), C(ImVec4(0.02f, 0.02f, 0.02f, 1)), label);
        }
    }
    TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(4.0f), std::min(y, max.y - S(30.0f))), C(p.textDim), T("spotify.premium_note"), colW);
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
        {Page::Spotify, kSpotifyLogo, "nav.spotify"},
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
    // glass (with a soft shadow under it: a blur, not a hard dark band)
    SoftLightOval(d, ImVec2((min.x + max.x) * 0.5f, max.y + S(4.0f)), (max.x - min.x) * 0.62f, h * 0.9f, ImVec4(0, 0, 0, 1), 0.45f);
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
        // its glow: soft (a blur), not rings
        SoftLightOval(d, ImVec2((lmin.x + lmax.x) * 0.5f, (lmin.y + lmax.y) * 0.5f), (lmax.x - lmin.x) * 0.75f, (lmax.y - lmin.y) * 0.9f, p.accent, 0.35f);
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
        g_flow.Start(SupportedMinecraft());
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
                g_flow.Start(SupportedMinecraft(), true);
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
void DrawSpotifyPage(ImDrawList* d, ImVec2 min, ImVec2 max);
void DrawSpotifyOverlays(ImDrawList* d, ImVec2 display);
void DrawSettingsSection(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc, const LocalData& data);
void DrawLyricsSection(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data);  // (below)
void DrawSettings(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc, const LocalData& data);  // (below)

// The other sections: a glass sheet that rises over the stage.
void DrawSheet(ImDrawList* d, ImVec2 display, const account::Info& acc, const LocalData& data) {
    const Palette& p = P();
    if (g_page != Page::Home) g_sheetPage = g_page;
    g_sheet += ((g_page != Page::Home ? 1.0f : 0.0f) - g_sheet) * std::min(1.0f, Dt() * 9.0f);
    if (g_sheet < 0.01f) return;
    const float s = EaseOutCubic(g_sheet);
    // SPOTIFY is a whole section, not a window: right on the album's living
    // background (the stage fades away under it).
    if (g_sheetPage == Page::Spotify || g_sheetPage == Page::Settings || g_sheetPage == Page::Lyrics) {
        const float rise = (1.0f - s) * S(30.0f);
        const ImVec2 cmin(S(40.0f), S(58.0f) + rise), cmax(display.x - S(40.0f), display.y - S(kDockBottom) - S(kDockH) - S(16.0f) + rise);
        const float saved = g_fade;
        g_fade = s;
        if (g_sheetPage == Page::Spotify) {
            DrawSpotifyPage(d, cmin, cmax);
        } else if (g_sheetPage == Page::Lyrics) {
            DrawLyricsSection(d, cmin, cmax, data);
        } else {
            DrawSettingsSection(d, cmin, cmax, acc, data);
        }
        g_fade = saved;
        return;
    }
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
        case Page::Spotify: DrawSpotifyPage(d, cmin, cmax); break;
        default: DrawSettings(d, cmin, cmax, acc, data); break;
    }
    d->PopClipRect();
    g_fade = saved;
}

// THE LYRICS VIEW: the song small at the top left, the lyrics big in the
// middle, a few controls at the bottom (the lyrics button closes it; Esc too).
void DrawLyricsView(ImDrawList* d, ImVec2 display, const media::Track& t, float alpha) {
    const Palette& p = P();
    const float rise = (1.0f - alpha) * S(24.0f);
    // the song, small
    const float art = S(58.0f);
    const ImVec2 artC(S(48.0f) + art * 0.5f, S(76.0f) + art * 0.5f + rise);
    DrawArtAt(d, artC, art, alpha, S(10.0f));
    const float tx = artC.x + art * 0.5f + S(14.0f);
    const float tw = std::min(S(420.0f), display.x * 0.4f);
    TextFit(d, g_fonts.semibold, ImVec2(tx, artC.y - g_fonts.semibold->FontSize - S(1.0f)), C(p.text, alpha), t.title, tw);
    TextFit(d, g_fonts.tiny, ImVec2(tx, artC.y + S(3.0f)), C(p.text, 0.65f * alpha), t.artist, tw);
    // the lyrics
    const float bottomCtl = display.y - S(kDockBottom) - S(kDockH) - S(72.0f);
    DrawLyricsLines(d, ImVec2(S(110.0f), S(150.0f) + rise), ImVec2(display.x - S(110.0f), bottomCtl - S(46.0f) + rise), t, alpha);
    // the controls
    const float savedFade = g_fade;
    g_fade *= alpha;
    const float cx = display.x * 0.5f, cy = bottomCtl + rise;
    if (RoundButton(d, "##lv_prev", ImVec2(cx - S(64.0f), cy), S(20.0f), icons::kSkipBackFilled, S(15.0f), false, t.canPrevious)) media::Previous();
    if (RoundButton(d, "##lv_play", ImVec2(cx, cy), S(26.0f), g_playMorph > 0.5f ? icons::kPlayFilled : icons::kPauseFilled, S(18.0f), true,
                    t.canPlayPause)) {
        media::PlayPause();
    }
    if (RoundButton(d, "##lv_next", ImVec2(cx + S(64.0f), cy), S(20.0f), icons::kSkipForwardFilled, S(15.0f), false, t.canNext)) media::Next();
    if (IconToggle(d, "##lv_close", ImVec2(cx + S(124.0f), cy), icons::kMicVocal, S(18.0f), true)) g_lyricsView = false;
    // a thin progress line under them (as before), that can be clicked or dragged
    ThinSeek(d, "##lv_seek", cx - S(130.0f), cx + S(130.0f), cy + S(38.0f), t, g_fade);
    g_fade = savedFade;
}

// THE KEYS (on Home, not while typing): Space = play / pause, Left / Right =
// 5 seconds back / forward (hold it to keep going), Ctrl + Left / Right = the
// previous / next song, Up / Down = the volume. Each one shows a small pill.
const char* g_flashIcon = nullptr;
char g_flashText[64] = "";
double g_flashAt = -10.0;

void Flash(const char* icon, const char* text) {
    g_flashIcon = icon;
    std::snprintf(g_flashText, sizeof(g_flashText), "%s", text);
    g_flashAt = ImGui::GetTime();
}

bool SpotifyConnected();  // (with the Spotify section)
bool SpotifySearchOpen();

// The same keys in the Spotify section: they control your Spotify itself
// (wherever it plays: this PC, Luma's own player, your phone...).
void HandleSpotifyKeys() {
    const spotify::Player pl = spotify::GetPlayer();
    if (!pl.valid || !pl.hasDevice) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        spotify::PlayerPlay(!pl.playing);
        Flash(pl.playing ? icons::kPauseFilled : icons::kPlayFilled, pl.playing ? T("music.paused") : T("key.play"));
    }
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            spotify::PlayerPrevious();
            Flash(icons::kSkipBackFilled, T("key.previous"));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            spotify::PlayerNext();
            Flash(icons::kSkipForwardFilled, T("key.next"));
        }
    } else {
        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true), right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        if ((left || right) && pl.durationMs > 0) {
            const double to = std::clamp(spotify::PositionSeconds(pl) + (right ? 5.0 : -5.0), 0.0, pl.durationMs / 1000.0);
            spotify::PlayerSeek(to);
            char text[32], label[64];
            FormatTime(to, text, sizeof(text));
            std::snprintf(label, sizeof(label), "%s 5 s  Â·  %s", right ? "+" : "-", text);
            Flash(right ? icons::kSkipForward : icons::kSkipBack, label);
        }
    }
    const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true), down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    if (up || down) {
        namespace eng = luma::launcher::spotifyengine;
        const bool luma = eng::GetState() == eng::State::Ready && pl.device.id == eng::DeviceId();
        const int now = luma ? eng::Volume() : pl.device.volume;
        if (now >= 0) {
            const int v = std::clamp(now + (up ? 5 : -5), 0, 100);
            if (luma) {
                eng::SetVolume(v);
            } else {
                spotify::PlayerVolume(v);
            }
            char label[64];
            std::snprintf(label, sizeof(label), T("key.volume"), v);
            Flash(icons::kVolume, label);
        }
    }
}

void HandleMusicKeys(const media::Track& t) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (g_page == Page::Spotify) {
        if (SpotifyConnected() && !SpotifySearchOpen()) HandleSpotifyKeys();
        return;
    }
    if (g_page != Page::Home || !t.present) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && t.canPlayPause) {
        media::PlayPause();
        Flash(t.playing ? icons::kPauseFilled : icons::kPlayFilled, t.playing ? T("music.paused") : T("key.play"));
    }
    const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true), right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && t.canPrevious) {
            media::Previous();
            Flash(icons::kSkipBackFilled, T("key.previous"));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && t.canNext) {
            media::Next();
            Flash(icons::kSkipForwardFilled, T("key.next"));
        }
    } else if ((left || right) && t.canSeek && t.duration > 0.0) {
        const double to = std::clamp(t.position + (right ? 5.0 : -5.0), 0.0, t.duration);
        media::Seek(to);
        char text[32];
        FormatTime(to, text, sizeof(text));
        char label[64];
        std::snprintf(label, sizeof(label), "%s 5 s  ·  %s", right ? "+" : "-", text);
        Flash(right ? icons::kSkipForward : icons::kSkipBack, label);
    }
    const bool upKey = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true), downKey = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    if ((upKey || downKey) && t.volume >= 0.0f) {
        const float v = std::clamp(t.volume + (upKey ? 0.05f : -0.05f), 0.0f, 1.0f);
        media::SetVolume(v);
        char label[64];
        std::snprintf(label, sizeof(label), T("key.volume"), static_cast<int>(std::lround(v * 100.0f)));
        Flash(icons::kVolume, label);
    }
}

// The pill of the last key: under the game's pill, it fades away.
void DrawFlash(ImDrawList* d, ImVec2 display) {
    const float age = static_cast<float>(ImGui::GetTime() - g_flashAt);
    if (age > 1.1f || g_flashIcon == nullptr) return;
    const Palette& p = P();
    const float a = std::clamp((1.1f - age) / 0.35f, 0.0f, 1.0f) * EaseOutCubic(std::min(1.0f, age * 8.0f));
    const float pop = 0.92f + 0.08f * EaseOutBack(std::min(1.0f, age * 6.0f));
    const ImVec2 ts = TextSize(g_fonts.semibold, g_flashText);
    const float w = (ts.x + S(56.0f)) * pop, h = S(40.0f) * pop;
    const ImVec2 c(display.x * 0.5f, S(78.0f));
    const ImVec2 a0(c.x - w * 0.5f, c.y - h * 0.5f), b0(c.x + w * 0.5f, c.y + h * 0.5f);
    d->AddRectFilled(a0, b0, Col(Mix(p.card, ImVec4(0, 0, 0, 1), 0.2f), 0.85f * a), h * 0.5f);
    d->AddRect(a0, b0, Col(ImVec4(1, 1, 1, 1), 0.12f * a), h * 0.5f);
    icons::Draw(d, g_flashIcon, ImVec2(a0.x + S(22.0f), c.y), S(16.0f), Col(p.accent, a), S(1.8f));
    Text(d, g_fonts.semibold, ImVec2(a0.x + S(40.0f), c.y - ts.y * 0.5f), Col(p.text, a), g_flashText);
}

// The whole stage (Home, with the sheet over it for the other sections).
void DrawStage(ImDrawList* d, ImVec2 display, const media::Track& t, const LocalData& data, const std::string& name, const account::Info& acc) {
    // (under a section the stage dims; under Spotify's whole section it fades away)
    const bool wholeSection = g_sheetPage == Page::Spotify || g_sheetPage == Page::Settings || g_sheetPage == Page::Lyrics;
    const float stageAlpha = (1.0f - 0.35f * EaseOutCubic(g_sheet)) * (wholeSection ? 1.0f - EaseOutCubic(g_sheet) : 1.0f);
    const ImVec2 smin(S(40.0f), S(70.0f));
    const ImVec2 smax(display.x - S(40.0f), display.y - S(kDockBottom) - S(kDockH) - S(70.0f));
    // (it comes in when the launcher opens)
    static float openIn = 0.0f;
    openIn = std::min(1.0f, openIn + Dt() * 1.4f);
    const float in = EaseOutCubic(openIn);
    const float savedFade = g_fade;
    g_fade = in;
    // The song, or (with the lyrics view open) the lyrics: one fades into the other.
    const float lv = EaseOutCubic(g_lyricsViewAnim);
    g_blockInput = g_sheet > 0.02f;  // (a section is up: the stage behind it is only a picture)
    if (lv < 0.98f) {
        const float drop = lv * S(30.0f);
        DrawStageSong(d, ImVec2(smin.x, smin.y + (1.0f - in) * S(20.0f) + drop), ImVec2(smax.x, smax.y + (1.0f - in) * S(20.0f) + drop), t,
                      stageAlpha * (1.0f - lv));
    }
    if (lv > 0.02f) DrawLyricsView(d, display, t, stageAlpha * lv);
    g_blockInput = false;
    g_fade = savedFade;
    DrawSheet(d, display, acc, data);
    DrawDock(d, display);
    DrawOrb(d, display, data);
    DrawOptionsMenu(d, t);  // (over everything)
    DrawSpotifyOverlays(d, display);
    HandleMusicKeys(t);
    DrawFlash(d, display);
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

// "Your Luma account": your styles and settings in the cloud (off unless you
// turn it on, see cloud_sync.h), and "Your data": what the server keeps,
// download it, delete it.
void DrawSettingsCloud(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc) {
    const Palette& p = P();
    const cloudsync::Info sync = cloudsync::Get();
    const bool signedIn = acc.state == account::State::SignedIn;
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("sync.title"));
    const float x = min.x + S(22.0f), right = max.x - S(22.0f), w = right - x;
    float y = min.y + S(62.0f);
    // the switch
    {
        const float h = SettingLabel(d, ImVec2(x, y), w - S(66.0f), T("sync.save"), T(signedIn ? "sync.save_hint" : "sync.needs_sign_in"));
        bool on = sync.enabled;
        const ImVec2 tp(right - S(46.0f), y + std::max(0.0f, (h - S(26.0f)) * 0.5f));
        if (signedIn || on) {
            if (Toggle("##sync_on", tp, &on)) cloudsync::SetEnabled(on);
        } else {  // (a switch you can't use yet: sign in first)
            d->AddRectFilled(tp, ImVec2(tp.x + S(46.0f), tp.y + S(26.0f)), C(p.text, 0.08f), S(13.0f));
            d->AddCircleFilled(ImVec2(tp.x + S(13.0f), tp.y + S(13.0f)), S(9.0f), C(p.textDim, 0.5f), 24);
        }
        y += std::max(h, S(30.0f)) + S(12.0f);
    }
    // how it's going
    if (sync.enabled) {
        char text[160];
        const ImVec4* color = &p.textDim;
        switch (sync.status) {
            case cloudsync::Status::Synced: {
                const long long ago = std::max(0LL, static_cast<long long>(std::time(nullptr)) - sync.lastSync);
                if (ago < 60) std::snprintf(text, sizeof(text), "%s", T("sync.saved_now"));
                else if (ago < 3600) std::snprintf(text, sizeof(text), T("sync.saved_min"), static_cast<int>(ago / 60));
                else std::snprintf(text, sizeof(text), T("sync.saved_hours"), static_cast<int>(ago / 3600));
                color = &p.success;
                break;
            }
            case cloudsync::Status::Working: std::snprintf(text, sizeof(text), "%s", T("sync.working")); break;
            case cloudsync::Status::Offline:
                std::snprintf(text, sizeof(text), "%s", T("sync.offline"));
                color = &p.warning;
                break;
            case cloudsync::Status::NeedsAccount:
                std::snprintf(text, sizeof(text), "%s", T("sync.needs_sign_in"));
                color = &p.warning;
                break;
            case cloudsync::Status::Error:
                std::snprintf(text, sizeof(text), "%s", T(sync.errorKey.empty() ? "sync.err_server" : sync.errorKey.c_str()));
                color = &p.danger;
                break;
            default: text[0] = 0; break;
        }
        if (sync.status == cloudsync::Status::Working) Spinner(d, ImVec2(x + S(7.0f), y + S(9.0f)), S(5.5f), C(p.textDim));
        else Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), *color);
        TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), text, w - S(20.0f));
        y += S(30.0f);
    }
    if (!signedIn) {
        if (!sync.actionKey.empty()) {  // (e.g. "your account was deleted")
            Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), sync.actionOk ? p.success : p.danger);
            TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), T(sync.actionKey.c_str()), w - S(20.0f));
        }
        return;
    }
    // your data
    y += S(6.0f);
    d->AddLine(ImVec2(x, y), ImVec2(right, y), C(p.border));
    y += S(14.0f);
    Text(d, g_fonts.semibold, ImVec2(x, y), C(p.text), T("sync.your_data"));
    y += g_fonts.semibold->FontSize + S(4.0f);
    y += TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("sync.what_is_kept"), w) + S(12.0f);
    // Deleting asks twice: the first click arms the button for 5 seconds.
    static double armedCloud = -100.0, armedAccount = -100.0;
    const double now = ImGui::GetTime();
    const bool cloudArmed = now - armedCloud < 5.0, accountArmed = now - armedAccount < 5.0;
    const bool idle = !sync.actionBusy;
    float bx = x, bw = 0.0f;
    if (Button("##sync_export", ImVec2(bx, y), T("sync.export"), false, &bw, idle)) cloudsync::ExportData();
    bx += bw + S(10.0f);
    const char* cloudLabel = T(cloudArmed ? "sync.delete_cloud_confirm" : "sync.delete_cloud");
    if (bx + TextSize(g_fonts.semibold, cloudLabel).x + S(32.0f) > right) {
        bx = x;
        y += S(46.0f);
    }
    if (Button("##sync_delcloud", ImVec2(bx, y), cloudLabel, false, &bw, idle)) {
        if (cloudArmed) {
            cloudsync::DeleteCloudCopy();
            armedCloud = -100.0;
        } else {
            armedCloud = now;
        }
    }
    bx += bw + S(10.0f);
    const char* accountLabel = T(accountArmed ? "sync.delete_account_confirm" : "sync.delete_account");
    if (bx + TextSize(g_fonts.semibold, accountLabel).x + S(32.0f) > right) {
        bx = x;
        y += S(46.0f);
    }
    if (Button("##sync_delacc", ImVec2(bx, y), accountLabel, accountArmed, &bw, idle)) {
        if (accountArmed) {
            cloudsync::DeleteAccount();
            armedAccount = -100.0;
        } else {
            armedAccount = now;
        }
    }
    y += S(48.0f);
    if (sync.actionBusy) {
        Spinner(d, ImVec2(x + S(7.0f), y + S(9.0f)), S(5.5f), C(p.textDim));
        TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), T("sync.working"), w - S(20.0f));
    } else if (!sync.actionKey.empty()) {
        Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), sync.actionOk ? p.success : p.danger);
        std::string text = T(sync.actionKey.c_str());
        if (!sync.exportPath.empty()) text += "  " + sync.exportPath;
        TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), text.c_str(), w - S(20.0f));
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


// =============================================================================
//  AJUSTES, THE WHOLE SECTION (part L3, design A): an index on the left (a
//  glass column with the sections) and the chosen one on the right, right on
//  the album's living background (no window over the stage).
// =============================================================================
int g_settingsTab = 0;      // 0 general, 1 music, 2 spotify, 3 account, 4 game, 5 about, 6 your year
float g_settingsTabIn = 1.0f;
float g_settingsPill = 0.0f;  // where the lit row of the index is (it slides)

// One row of a settings card: a title, a hint under it, and the control at the right.
float SettingRow(ImDrawList* d, float x, float y, float w, const char* title, const char* hint, float controlW) {
    return SettingLabel(d, ImVec2(x, y), w - controlW - S(20.0f), title, hint);
}

void DrawSettingsMusic(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    static bool arrows = false;
    if (!arrows) {  // (the arrows of the keys come from another font)
        arrows = true;
        render::fontfallback::Need("\xE2\x86\x90\xE2\x86\x91\xE2\x86\x92\xE2\x86\x93");
    }
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("set.music"));
    const float x = min.x + S(22.0f), right = max.x - S(22.0f), w = right - x;
    float y = min.y + S(62.0f);
    const float gap = S(22.0f);
    // the living background
    {
        const float h = SettingRow(d, x, y, w, T("set.live_bg"), T("set.live_bg_hint"), S(46.0f));
        if (Toggle("##set_livebg", ImVec2(right - S(46.0f), y + std::max(0.0f, (h - S(26.0f)) * 0.5f)), &g_liveBackground)) SaveConfig();
        y += std::max(h, S(30.0f)) + gap;
    }
    d->AddLine(ImVec2(x, y - gap * 0.5f), ImVec2(right, y - gap * 0.5f), C(p.border));
    // the lyrics on Home
    {
        const float h = SettingRow(d, x, y, w, T("set.home_lyrics"), T("set.home_lyrics_hint"), S(46.0f));
        if (Toggle("##set_homelyrics", ImVec2(right - S(46.0f), y + std::max(0.0f, (h - S(26.0f)) * 0.5f)), &g_homeLyrics)) SaveConfig();
        y += std::max(h, S(30.0f)) + gap;
    }
    d->AddLine(ImVec2(x, y - gap * 0.5f), ImVec2(right, y - gap * 0.5f), C(p.border));
    // the lyrics' timing (the same setting as the game's "Offset")
    {
        const float h = SettingRow(d, x, y, w, T("opt.offset"), T("set.offset_hint"), S(150.0f));
        const float off = flowcfg::g_state.offset;
        char value[16];
        std::snprintf(value, sizeof(value), "%+.1f s", off);
        const ImVec2 vs = TextSize(g_fonts.semibold, value);
        const float cy = y + std::max(h, S(30.0f)) * 0.5f;
        const float plusX = right - S(16.0f), minusX = plusX - S(40.0f) - vs.x - S(10.0f);
        Text(d, g_fonts.semibold, ImVec2((minusX + plusX) * 0.5f - vs.x * 0.5f, cy - vs.y * 0.5f), C(p.text), value);
        if (IconToggle(d, "##set_off_minus", ImVec2(minusX, cy), icons::kMinus, S(15.0f), false)) flowcfg::SetOffset(std::clamp(off - 0.1f, -5.0f, 5.0f));
        if (IconToggle(d, "##set_off_plus", ImVec2(plusX, cy), icons::kPlus, S(15.0f), false)) flowcfg::SetOffset(std::clamp(off + 0.1f, -5.0f, 5.0f));
        y += std::max(h, S(30.0f)) + gap;
    }
    d->AddLine(ImVec2(x, y - gap * 0.5f), ImVec2(right, y - gap * 0.5f), C(p.border));
    // the keys (Home)
    {
        Text(d, g_fonts.semibold, ImVec2(x, y), C(p.text), T("set.keys"));
        y += g_fonts.semibold->FontSize + S(10.0f);
        struct KeyRow {
            const char* keys;
            const char* what;
        };
        const KeyRow rows[] = {{"Espacio / Space", "set.key_pause"}, {"\xE2\x86\x90  \xE2\x86\x92", "set.key_seek"},
                               {"Ctrl + \xE2\x86\x90  \xE2\x86\x92", "set.key_song"}, {"\xE2\x86\x91  \xE2\x86\x93", "set.key_volume"},
                               {"Ctrl + K", "set.key_search"}};
        for (const KeyRow& r : rows) {
            const ImVec2 ks = TextSize(g_fonts.semibold, r.keys);
            d->AddRectFilled(ImVec2(x, y - S(3.0f)), ImVec2(x + ks.x + S(18.0f), y + ks.y + S(3.0f)), C(p.text, 0.08f), S(7.0f));
            Text(d, g_fonts.semibold, ImVec2(x + S(9.0f), y), C(p.text), r.keys);
            Text(d, g_fonts.regular, ImVec2(x + std::max(ks.x + S(30.0f), S(170.0f)), y + S(1.0f)), C(p.textDim), T(r.what));
            y += S(34.0f);
        }
    }
}

void DrawSettingsSpotify(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    DrawSpotifyLogo(d, ImVec2(min.x + S(32.0f), min.y + S(28.0f)), S(24.0f), g_fade);
    Text(d, g_fonts.semibold, ImVec2(min.x + S(52.0f), min.y + S(18.0f)), C(p.text), "Spotify");
    const float x = min.x + S(22.0f), right = max.x - S(22.0f), w = right - x;
    float y = min.y + S(62.0f);
    const spotify::Status st = spotify::GetStatus();
    float bw = 0.0f;
    if (st.state != spotify::State::Connected) {
        y += TextWrap(d, g_fonts.regular, ImVec2(x, y), C(p.textDim), T("set.sp_not_connected"), w) + S(14.0f);
        if (SmallPill(d, "##set_sp_go", ImVec2(x, y), nullptr, T("set.sp_go"), true, &bw)) GoTo(Page::Spotify);
        return;
    }
    // your account
    {
        std::string who = st.user + (st.premium == 1 ? "  \xC2\xB7  Premium" : (st.premium == 0 ? "  \xC2\xB7  Free" : ""));
        Text(d, g_fonts.title, ImVec2(x, y), C(p.text), who.c_str());
        y += g_fonts.title->FontSize + S(4.0f);
        Text(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("spotify.shared"));
        if (SmallPill(d, "##set_sp_off", ImVec2(right - TextSize(g_fonts.semibold, T("spotify.disconnect")).x - S(28.0f), y - S(26.0f)), nullptr,
                      T("spotify.disconnect"), false, &bw)) {
            spotify::Disconnect();
        }
        y += g_fonts.tiny->FontSize + S(22.0f);
    }
    d->AddLine(ImVec2(x, y - S(11.0f)), ImVec2(right, y - S(11.0f)), C(p.border));
    // Luma's own player: its card, and the sound quality
    y += sp::LumaPlayerCard(d, x, y, w, g_fade) + S(18.0f);
    {
        namespace eng = luma::launcher::spotifyengine;
        const char* labels[2] = {T("set.sp_q_normal"), T("set.sp_q_high")};
        int q = eng::HighQuality() ? 1 : 0;
        const float sw = SegmentedWidth(labels, 2);
        const float h = SettingRow(d, x, y, w, T("set.sp_quality"), T("set.sp_quality_hint"), sw);
        if (Segmented("##set_sp_quality", ImVec2(right - sw, y + std::max(0.0f, (h - S(38.0f)) * 0.5f)), labels, 2, &q)) eng::SetHighQuality(q == 1);
        y += std::max(h, S(38.0f)) + S(16.0f);
    }
    TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("set.sp_note"), w);
}

void DrawSettingsGame(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data) {
    const Palette& p = P();
    const float gap = S(kGap);
    // the game: its version and whether Luma is in it
    const ImVec2 a = min, b(max.x, min.y + S(118.0f));
    Card(d, a, b);
    SectionTitle(d, ImVec2(a.x + S(20.0f), a.y + S(18.0f)), "Minecraft");
    {
        char text[160];
        const bool loaded = data.game.lumaLoaded;
        if (data.game.gameVersion.empty()) {
            std::snprintf(text, sizeof(text), "%s", T("home.not_installed"));
        } else {
            std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", data.game.gameVersion.c_str(), loaded ? T("home.luma_loaded") : T("home.ready_to_play"));
        }
        Dot(d, ImVec2(a.x + S(30.0f), a.y + S(72.0f)), loaded ? p.success : p.accent);
        Text(d, g_fonts.semibold, ImVec2(a.x + S(44.0f), a.y + S(72.0f) - g_fonts.semibold->FontSize * 0.5f), C(p.text), text);
    }
    const float y1 = b.y + gap;
    DrawSettingsFolders(d, ImVec2(min.x, y1), ImVec2(max.x, y1 + S(126.0f)));
    const float y2 = y1 + S(126.0f) + gap;
    if (max.y - y2 > S(120.0f)) DrawSettingsShortcuts(d, ImVec2(min.x, y2), ImVec2(max.x, std::min(max.y, y2 + S(170.0f))));
}

// =============================================================================
//  "YOUR YEAR IN LUMA" (listening.h): what you listened to while playing.
//  The card is drawn by one function for both the preview here and the
//  picture you save (a PNG of 1080 x 1350, the size social networks like).
// =============================================================================
namespace yearui {

int g_year = 0;
double g_armedLocal = -100.0, g_armedCloud = -100.0;
std::string g_saved;      // where the last picture was saved
bool g_saveFailed = false;
int g_cloudAskedYear = 0;
int g_passVersion = -1;

std::string FitText(ImFont* font, float size, float width, const std::string& text) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= width) return text;
    std::string s = text;
    const char* dots = "\xE2\x80\xA6";
    while (!s.empty()) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
        if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, (s + dots).c_str()).x <= width) return s + dots;
    }
    return dots;
}

void Put(ImDrawList* d, ImFont* font, float size, ImVec2 pos, ImU32 color, const std::string& text) {
    d->AddText(font, size, pos, color, text.c_str());
}

std::string Hours(double seconds) {
    char out[48];
    if (seconds < 3600.0) std::snprintf(out, sizeof(out), "%d min", static_cast<int>(seconds / 60.0));
    else if (seconds < 36000.0) std::snprintf(out, sizeof(out), "%.1f h", seconds / 3600.0);
    else std::snprintf(out, sizeof(out), "%d h", static_cast<int>(seconds / 3600.0 + 0.5));
    return out;
}

// Numbers in PIXELS (5 x 7 blocks, like Minecraft's letters), drawn block by
// block: sharp at any size, on the screen and in the saved picture.
const unsigned char* PixelGlyph(char c, int& width) {
    static const unsigned char k0[7] = {14, 17, 19, 21, 25, 17, 14}, k1[7] = {4, 12, 4, 4, 4, 4, 14}, k2[7] = {14, 17, 1, 2, 4, 8, 31},
                               k3[7] = {30, 1, 1, 14, 1, 1, 30}, k4[7] = {2, 6, 10, 18, 31, 2, 2}, k5[7] = {31, 16, 30, 1, 1, 17, 14},
                               k6[7] = {6, 8, 16, 30, 17, 17, 14}, k7[7] = {31, 1, 2, 4, 8, 8, 8}, k8[7] = {14, 17, 17, 14, 17, 17, 14},
                               k9[7] = {14, 17, 17, 15, 1, 2, 12}, kh[7] = {16, 16, 22, 25, 17, 17, 17}, km[7] = {0, 0, 26, 21, 21, 17, 17},
                               ki[7] = {4, 0, 12, 4, 4, 4, 14}, kn[7] = {0, 0, 22, 25, 17, 17, 17}, kdot[7] = {0, 0, 0, 0, 0, 12, 12};
    width = 5;
    switch (c) {
        case '0': return k0;
        case '1': return k1;
        case '2': return k2;
        case '3': return k3;
        case '4': return k4;
        case '5': return k5;
        case '6': return k6;
        case '7': return k7;
        case '8': return k8;
        case '9': return k9;
        case 'h': return kh;
        case 'm': return km;
        case 'i': return ki;
        case 'n': return kn;
        case '.': case ',': return kdot;
        default: width = 3; return nullptr;  // (a space)
    }
}

float PixelWidth(const std::string& text, float cell) {
    float w = 0.0f;
    for (char c : text) {
        int gw = 0;
        PixelGlyph(c, gw);
        w += (gw + 1) * cell;
    }
    return std::max(0.0f, w - cell);
}

// (Clean, without a shadow: on the black it would only smudge the blocks.)
void PixelText(ImDrawList* d, ImVec2 pos, float cell, ImU32 color, const std::string& text) {
    for (int pass = 1; pass < 2; ++pass) {
        const ImU32 col = pass == 0 ? IM_COL32(0, 0, 0, 90) : color;
        const float off = pass == 0 ? cell : 0.0f;
        float x = pos.x;
        for (char c : text) {
            int gw = 0;
            const unsigned char* g = PixelGlyph(c, gw);
            if (g != nullptr) {
                for (int row = 0; row < 7; ++row) {
                    for (int col2 = 0; col2 < 5; ++col2) {
                        if (g[row] & (16 >> col2)) {
                            const ImVec2 a(x + col2 * cell + off, pos.y + row * cell + off);
                            d->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), col);
                        }
                    }
                }
            }
            x += (gw + 1) * cell;
        }
    }
}

// The card, in the rectangle a..b (4:5). Everything is measured in "u"
// (1/540 of its width), so it looks the same small or big. OLED black, with
// soft gradients of Luma's own color (the logo's ivory and its warm stone
// tones, the same as the website's living background). THE HILL IS YOUR
// YEAR: each of its twelve columns of blocks is a month, as tall as the
// music you heard in it; the blocks fade into the black.
void DrawCard(ImDrawList* d, ImVec2 a, ImVec2 b, const listening::Summary& s) {
    const float W = b.x - a.x, H = b.y - a.y, u = W / 540.0f;
    const ImU32 ivory = IM_COL32(233, 230, 223, 255), dim = IM_COL32(233, 230, 223, 150), faint = IM_COL32(233, 230, 223, 80);
    const ImVec4 stone(0.604f, 0.561f, 0.486f, 1.0f), stoneDark(0.478f, 0.447f, 0.400f, 1.0f), ivoryV(0.913f, 0.902f, 0.874f, 1.0f);
    d->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
    // the light: soft gradients of Luma's color on the black
    d->PushClipRect(a, b, true);
    SoftLightOval(d, ImVec2(a.x + W * 0.20f, a.y + H * 0.16f), W * 0.62f, H * 0.36f, stone, 0.95f);
    SoftLightOval(d, ImVec2(a.x + W * 0.92f, a.y + H * 0.06f), W * 0.46f, H * 0.26f, stoneDark, 0.70f);
    SoftLightOval(d, ImVec2(a.x + W * 0.55f, a.y + H * 0.40f), W * 0.70f, H * 0.22f, ivoryV, 0.16f);
    SoftLightOval(d, ImVec2(a.x + W * 0.08f, a.y + H * 0.98f), W * 0.55f, H * 0.30f, stoneDark, 0.55f);
    SoftLightOval(d, ImVec2(a.x + W * 0.95f, a.y + H * 0.88f), W * 0.40f, H * 0.26f, stone, 0.35f);
    d->PopClipRect();
    ImFont* semi = g_fonts.semibold;
    ImFont* reg = g_fonts.regular;
    const float m = u * 38.0f;
    const float x = a.x + m, right = b.x - m;
    // --- top: LUMA, the year, what it is ---
    d->AddText(semi, u * 15.0f, ImVec2(x, a.y + u * 40.0f), dim, "LUMA");
    {
        const char* from = T(s.fromCloud ? "year.card_all_pcs" : "year.card_this_pc");
        const float fw = reg->CalcTextSizeA(u * 12.0f, FLT_MAX, 0.0f, from).x;
        d->AddText(reg, u * 12.0f, ImVec2(right - fw, a.y + u * 42.0f), faint, from);
    }
    char year[16];
    std::snprintf(year, sizeof(year), "%d", s.year);
    PixelText(d, ImVec2(x, a.y + u * 72.0f), u * 8.0f, ivory, year);
    d->AddText(semi, u * 19.0f, ImVec2(x, a.y + u * 138.0f), ivory, T("year.card_title"));
    // --- the hill of your year ---
    const float ground = a.y + u * 300.0f;
    double most = 0.0;
    int best = -1;
    for (int i = 0; i < 12; ++i) {
        if (s.months[i] > most) {
            most = s.months[i];
            best = i;
        }
    }
    const float colW = (right - x) / 12.0f;
    const float gapB = std::max(1.0f, u * 1.5f);
    const float blk = (colW - gapB) / 3.0f;
    constexpr int kRows = 7;
    static const char* initials[3] = {"JFMAMJJASOND", "EFMAMJJASOND", "JFMAMJJASOND"};
    const char* letters = initials[static_cast<int>(i18n::CurrentLanguage()) % 3];
    for (int i = 0; i < 12; ++i) {
        const float cx = x + colW * i;
        const int rows = most > 0.0 && s.months[i] > 0.0 ? std::max(1, static_cast<int>(std::round(s.months[i] / most * kRows))) : 0;
        for (int r = 0; r < rows; ++r) {
            // brighter at the top of each column, fading into the black at the bottom
            const float t = rows > 1 ? static_cast<float>(r) / static_cast<float>(rows - 1) : 1.0f;
            const float alpha = (i == best ? 0.22f : 0.12f) + t * (i == best ? 0.78f : 0.48f);
            const float y1 = ground - r * (blk + gapB), y0 = y1 - blk;
            for (int c = 0; c < 3; ++c) {
                const float bx = cx + c * (blk + gapB * 0.5f);
                d->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + blk - gapB * 0.5f, y1), C(ivoryV, alpha * ((r + c) % 2 == 0 ? 1.0f : 0.86f)));
            }
        }
        const char letter[2] = {letters[i], 0};
        const float lw = reg->CalcTextSizeA(u * 12.0f, FLT_MAX, 0.0f, letter).x;
        d->AddText(reg, u * 12.0f, ImVec2(cx + (colW - gapB) * 0.5f - lw * 0.5f, ground + u * 10.0f), i == best ? ivory : faint, letter);
    }
    d->AddRectFilledMultiColor(ImVec2(x, ground + u * 2.0f), ImVec2(right, ground + u * 3.0f), C(ivoryV, 0.05f), C(ivoryV, 0.25f),
                               C(ivoryV, 0.25f), C(ivoryV, 0.05f));
    // --- the numbers ---
    float y = ground + u * 46.0f;
    PixelText(d, ImVec2(x, y), u * 6.0f, ivory, Hours(s.seconds));
    d->AddText(reg, u * 14.0f, ImVec2(x, y + u * 48.0f), dim, T("year.card_hours"));
    {  // plays, songs, artists, at the right
        const int values[3] = {s.plays, s.songs, s.artists};
        const char* keys[3] = {"year.card_plays", "year.card_song_count", "year.card_artist_count"};
        float rx = right;
        for (int i = 2; i >= 0; --i) {
            char n[16];
            std::snprintf(n, sizeof(n), "%d", values[i]);
            const float nw = PixelWidth(n, u * 3.0f);
            const float lw = reg->CalcTextSizeA(u * 12.0f, FLT_MAX, 0.0f, T(keys[i])).x;
            const float cw = std::max(nw, lw);
            PixelText(d, ImVec2(rx - cw + (cw - nw) * 0.5f, y + u * 12.0f), u * 3.0f, ivory, n);
            d->AddText(reg, u * 12.0f, ImVec2(rx - cw + (cw - lw) * 0.5f, y + u * 40.0f), dim, T(keys[i]));
            rx -= cw + u * 26.0f;
        }
    }
    y += u * 86.0f;
    // --- top songs / top artists: glass of Luma's color on the black ---
    const float gap = u * 14.0f;
    const float cw = (right - x + u * 8.0f - gap) * 0.5f;
    const float left = x - u * 4.0f;
    const float ch = b.y - u * 34.0f - y;
    for (int col = 0; col < 2; ++col) {
        const ImVec2 ca(left + col * (cw + gap), y), cb(ca.x + cw, y + ch);
        const float r = u * 16.0f;
        d->AddRectFilled(ca, cb, C(ivoryV, 0.035f), r);
        d->AddRect(ca, cb, C(ivoryV, 0.12f), r, 0, std::max(1.0f, u));
        const float px = ca.x + u * 18.0f;
        d->AddText(semi, u * 14.0f, ImVec2(px, ca.y + u * 16.0f), ivory, T(col == 0 ? "year.card_songs" : "year.card_artists"));
        const auto& list = col == 0 ? s.topSongs : s.topArtists;
        float ry = ca.y + u * 46.0f;
        const float rowH = (cb.y - ry - u * 8.0f) / 5.0f;
        if (list.empty()) d->AddText(reg, u * 14.0f, ImVec2(px, ry), faint, "\xE2\x80\x94");
        for (std::size_t i = 0; i < list.size() && i < 5; ++i) {
            char n[4];
            std::snprintf(n, sizeof(n), "%zu", i + 1);
            PixelText(d, ImVec2(px, ry + u * 4.0f), u * 2.4f, i == 0 ? ivory : faint, n);
            const float tx2 = px + u * 22.0f, tw = cb.x - tx2 - u * 14.0f;
            const std::string main = col == 0 ? list[i].title : list[i].artist;
            const std::string sub = col == 0 ? (list[i].artist.empty() ? Hours(list[i].seconds) : list[i].artist) : Hours(list[i].seconds);
            d->AddText(semi, u * 14.0f, ImVec2(tx2, ry), ivory, FitText(semi, u * 14.0f, tw, main).c_str());
            d->AddText(reg, u * 12.0f, ImVec2(tx2, ry + u * 18.0f), dim, FitText(reg, u * 12.0f, tw, sub).c_str());
            ry += rowH;
        }
    }
}

// Draws the card into a picture and saves it as a PNG (with the GPU, the same
// way the window is drawn; WIC writes the file).
bool SavePng(const listening::Summary& s, const fs::path& file) {
    constexpr int kW = 1080, kH = 1350;
    if (g_device == nullptr) return false;
    ID3D11DeviceContext* ctx = nullptr;
    g_device->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kW;
    desc.Height = kH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &target)) &&
              SUCCEEDED(g_device->CreateRenderTargetView(target, nullptr, &rtv));
    if (ok) {
        D3D11_TEXTURE2D_DESC sd = desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &staging));
    }
    std::vector<std::uint8_t> pixels;
    if (ok) {
        // the card, as its own little list of drawings
        ImDrawList list(ImGui::GetDrawListSharedData());
        list._ResetForNewFrame();
        list.PushTextureID(ImGui::GetIO().Fonts->TexID);
        list.PushClipRect(ImVec2(0, 0), ImVec2(static_cast<float>(kW), static_cast<float>(kH)));
        DrawCard(&list, ImVec2(0, 0), ImVec2(static_cast<float>(kW), static_cast<float>(kH)), s);
        ImDrawData data;
        data.Valid = true;
        data.CmdLists.push_back(&list);
        data.CmdListsCount = 1;
        data.TotalVtxCount = list.VtxBuffer.Size;
        data.TotalIdxCount = list.IdxBuffer.Size;
        data.DisplayPos = ImVec2(0, 0);
        data.DisplaySize = ImVec2(static_cast<float>(kW), static_cast<float>(kH));
        data.FramebufferScale = ImVec2(1, 1);
        ID3D11RenderTargetView* oldRtv = nullptr;
        ID3D11DepthStencilView* oldDsv = nullptr;
        ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
        const float clear[4] = {0, 0, 0, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(&data);
        ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
        if (oldRtv) oldRtv->Release();
        if (oldDsv) oldDsv->Release();
        ctx->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            pixels.resize(static_cast<std::size_t>(kW) * kH * 4);
            for (int row = 0; row < kH; ++row) {
                const std::uint8_t* src = static_cast<const std::uint8_t*>(map.pData) + static_cast<std::size_t>(row) * map.RowPitch;
                std::uint8_t* dst = pixels.data() + static_cast<std::size_t>(row) * kW * 4;
                for (int i = 0; i < kW; ++i) {  // RGBA -> BGRA (what the PNG writer takes), always opaque
                    dst[i * 4 + 0] = src[i * 4 + 2];
                    dst[i * 4 + 1] = src[i * 4 + 1];
                    dst[i * 4 + 2] = src[i * 4 + 0];
                    dst[i * 4 + 3] = 255;
                }
            }
            ctx->Unmap(staging, 0);
        } else {
            ok = false;
        }
    }
    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (target) target->Release();
    if (ctx) ctx->Release();
    if (!ok || pixels.empty()) return false;
    // the PNG file
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(kW, kH)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
         format == GUID_WICPixelFormat32bppBGRA &&
         SUCCEEDED(frame->WritePixels(kH, kW * 4, static_cast<UINT>(pixels.size()), pixels.data())) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

}  // namespace yearui

void DrawSettingsYear(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc) {
    using namespace yearui;
    const Palette& p = P();
    const listening::Info li = listening::Get();
    const bool signedIn = acc.state == account::State::SignedIn && !account::LumaPass().empty();
    std::time_t clock = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &clock);
    const int thisYear = local.tm_year + 1900;
    if (g_year == 0) g_year = thisYear;
    // Which summary: your account's (all your PCs) if you save it there, else this PC's.
    const bool useCloud = li.upload && signedIn;
    if (account::LumaPassVersion() != g_passVersion) {
        g_passVersion = account::LumaPassVersion();
        g_cloudAskedYear = 0;
    }
    if (useCloud && g_cloudAskedYear != g_year) {
        g_cloudAskedYear = g_year;
        listening::RefreshCloud(g_year);
    }
    bool cloudOk = false;
    listening::Summary s = useCloud ? listening::Cloud(g_year, cloudOk) : listening::Summary{};
    if (!cloudOk) s = listening::Local(g_year);
    // --- the card (left) ---
    const float cardH = std::min(max.y - min.y, S(600.0f));
    const float cardW = cardH * 0.8f;
    const ImVec2 ca(min.x, min.y), cb(min.x + cardW, min.y + cardH);
    d->PushClipRect(ca, cb, true);
    DrawCard(d, ca, cb, s);
    d->PopClipRect();
    d->AddRect(ca, cb, C(ImVec4(1, 1, 1, 1), 0.10f), 0.0f);
    // --- the controls (right) ---
    const ImVec2 ra(cb.x + S(kGap), min.y), rb(max.x, std::min(max.y, min.y + S(600.0f)));
    if (rb.x - ra.x < S(260.0f)) return;
    Card(d, ra, rb);
    SectionTitle(d, ImVec2(ra.x + S(20.0f), ra.y + S(18.0f)), T("year.title"));
    const float x = ra.x + S(22.0f), right = rb.x - S(22.0f), w = right - x;
    // the year: < 2026 >
    {
        char text[16];
        std::snprintf(text, sizeof(text), "%d", g_year);
        const ImVec2 ts = TextSize(g_fonts.semibold, text);
        const float cy = ra.y + S(28.0f);
        if (IconToggle(d, "##year_next", ImVec2(right - S(12.0f), cy), icons::kChevronRight, S(15.0f), false, g_year < thisYear)) ++g_year;
        Text(d, g_fonts.semibold, ImVec2(right - S(30.0f) - ts.x, cy - ts.y * 0.5f), C(p.text), text);
        if (IconToggle(d, "##year_prev", ImVec2(right - S(48.0f) - ts.x, cy), icons::kBack, S(15.0f), false, g_year > 2025)) --g_year;
    }
    float y = ra.y + S(62.0f);
    y += TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("year.what"), w) + S(16.0f);
    // record
    {
        bool on = li.recording;
        const float h = SettingLabel(d, ImVec2(x, y), w - S(66.0f), T("year.record"), nullptr);
        if (Toggle("##year_rec", ImVec2(right - S(46.0f), y + std::max(0.0f, (h - S(26.0f)) * 0.5f)), &on)) listening::SetRecording(on);
        y += std::max(h, S(30.0f)) + S(4.0f);
        std::string status;
        if (!li.recording) status = T("year.rec_off");
        else if (!li.gameRunning) status = T("year.rec_waiting");
        else if (li.nowTitle.empty()) status = T("year.rec_silence");
        else status = std::string(T("year.rec_now")) + " " + li.nowTitle;
        Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), li.recording && li.gameRunning && !li.nowTitle.empty() ? p.success : p.textDim);
        TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), status.c_str(), w - S(20.0f));
        y += S(36.0f);
    }
    d->AddLine(ImVec2(x, y - S(6.0f)), ImVec2(right, y - S(6.0f)), C(p.border));
    y += S(6.0f);
    // save in my account
    {
        bool on = li.upload;
        const float h = SettingLabel(d, ImVec2(x, y), w - S(66.0f), T("year.upload"), T(signedIn ? "year.upload_hint" : "sync.needs_sign_in"));
        const ImVec2 tp(right - S(46.0f), y + std::max(0.0f, (h - S(26.0f)) * 0.5f));
        if (signedIn || on) {
            if (Toggle("##year_up", tp, &on)) {
                listening::SetUpload(on);
                g_cloudAskedYear = 0;
            }
        }
        y += std::max(h, S(30.0f)) + S(6.0f);
        if (li.upload && signedIn) {
            const bool bad = !li.uploadKey.empty();
            char text[160];
            if (bad) std::snprintf(text, sizeof(text), "%s", T(li.uploadKey.c_str()));
            else if (li.lastUpload == 0) std::snprintf(text, sizeof(text), "%s", T("sync.working"));
            else std::snprintf(text, sizeof(text), "%s", T("year.uploaded"));
            Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), bad ? p.warning : p.success);
            TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), text, w - S(20.0f));
            y += S(32.0f);
        }
    }
    y += S(8.0f);
    // the buttons
    const double now = ImGui::GetTime();
    float bx = x, bw = 0.0f;
    if (Button("##year_png", ImVec2(bx, y), T("year.save_image"), true, &bw)) {
        PWSTR raw = nullptr;
        fs::path dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &raw)) && raw) dir = fs::path(raw) / L"Luma";
        CoTaskMemFree(raw);
        if (dir.empty()) dir = DataDir();
        const fs::path file = dir / (L"Luma " + std::to_wstring(g_year) + L".png");
        g_saveFailed = !SavePng(s, file);
        if (!g_saveFailed) {
            const std::u8string u8 = file.u8string();
            g_saved.assign(u8.begin(), u8.end());
            const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    bx += bw + S(10.0f);
    const bool armedLocal = now - g_armedLocal < 5.0;
    const char* localLabel = T(armedLocal ? "sync.delete_cloud_confirm" : "year.delete_local");
    if (bx + TextSize(g_fonts.semibold, localLabel).x + S(32.0f) > right) {
        bx = x;
        y += S(46.0f);
    }
    if (Button("##year_dellocal", ImVec2(bx, y), localLabel, false, &bw)) {
        if (armedLocal) {
            listening::DeleteLocal();
            g_armedLocal = -100.0;
        } else {
            g_armedLocal = now;
        }
    }
    bx += bw + S(10.0f);
    if (signedIn) {
        const bool armedCloud = now - g_armedCloud < 5.0;
        const char* cloudLabel = T(armedCloud ? "sync.delete_cloud_confirm" : "year.delete_cloud");
        if (bx + TextSize(g_fonts.semibold, cloudLabel).x + S(32.0f) > right) {
            bx = x;
            y += S(46.0f);
        }
        if (Button("##year_delcloud", ImVec2(bx, y), cloudLabel, false, &bw)) {
            if (armedCloud) {
                listening::DeleteCloud();
                g_armedCloud = -100.0;
                g_cloudAskedYear = 0;
            } else {
                g_armedCloud = now;
            }
        }
    }
    y += S(50.0f);
    // what happened
    if (g_saveFailed || !g_saved.empty()) {
        Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), g_saveFailed ? p.danger : p.success);
        const std::string text = g_saveFailed ? std::string(T("year.save_failed")) : std::string(T("year.saved")) + "  " + g_saved;
        TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), text.c_str(), w - S(20.0f));
        y += S(28.0f);
    }
    if (!li.actionKey.empty()) {
        Dot(d, ImVec2(x + S(7.0f), y + S(9.0f)), li.actionOk ? p.success : p.danger);
        TextFit(d, g_fonts.regular, ImVec2(x + S(20.0f), y), C(p.text), T(li.actionKey.c_str()), w - S(20.0f));
    }
}

void DrawSettingsAboutAll(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    const float gap = S(kGap);
    DrawSettingsAbout(d, min, ImVec2(max.x, min.y + S(250.0f)));
    const float y1 = min.y + S(250.0f) + gap;
    // what Luma is made with (and their licenses)
    const ImVec2 a(min.x, y1), b(max.x, std::min(max.y, y1 + S(200.0f)));
    Card(d, a, b);
    SectionTitle(d, ImVec2(a.x + S(20.0f), a.y + S(18.0f)), T("set.credits"));
    TextWrap(d, g_fonts.tiny, ImVec2(a.x + S(22.0f), a.y + S(56.0f)), C(p.textDim), T("set.credits_text"), b.x - a.x - S(44.0f));
    const float y2 = b.y + gap;
    if (max.y - y2 > S(110.0f)) DrawSettingsLinks(d, ImVec2(min.x, y2), ImVec2(max.x, std::min(max.y, y2 + S(150.0f))));
}

// THE INDEX of a whole section (Settings, Lyrics): a glass column with the
// title and the parts; the lit row slides to the chosen one. (icon nullptr =
// Spotify's logo.) True when the chosen part changed.
struct IndexEntry {
    const char* icon;
    const char* key;
};

bool DrawSectionIndex(ImDrawList* d, ImVec2 na, ImVec2 nb, const char* title, const IndexEntry* entries, int count, int* tab, float* pill,
                      const char* idPrefix) {
    const Palette& p = P();
    bool changed = false;
    d->AddRectFilled(na, nb, C(Mix(p.card, ImVec4(0, 0, 0, 1), 0.15f), 0.45f + 0.35f * (1.0f - g_glass)), S(18.0f));
    d->AddRect(na, nb, C(ImVec4(1, 1, 1, 1), 0.12f), S(18.0f));
    Text(d, g_fonts.title, ImVec2(na.x + S(20.0f), na.y + S(18.0f)), C(p.text), title);
    const float rowH = S(46.0f);
    const float listY = na.y + S(70.0f);
    *pill += (static_cast<float>(*tab) - *pill) * std::min(1.0f, Dt() * 14.0f);
    {
        const float py = listY + *pill * rowH;
        d->AddRectFilled(ImVec2(na.x + S(10.0f), py), ImVec2(nb.x - S(10.0f), py + rowH - S(6.0f)), C(p.accent, 0.22f), S(12.0f));
        d->AddRectFilled(ImVec2(na.x + S(10.0f), py + S(10.0f)), ImVec2(na.x + S(13.0f), py + rowH - S(16.0f)), C(p.accent), S(2.0f));
    }
    for (int i = 0; i < count; ++i) {
        const ImVec2 ra(na.x + S(10.0f), listY + rowH * static_cast<float>(i)), rb(nb.x - S(10.0f), ra.y + rowH - S(6.0f));
        char id[32];
        std::snprintf(id, sizeof(id), "##%s_tab%d", idPrefix, i);
        float hover = 0.0f;
        if (Hit(id, ra, rb, &hover) && *tab != i) {
            *tab = i;
            changed = true;
        }
        const float lit = std::clamp(1.0f - std::fabs(*pill - static_cast<float>(i)), 0.0f, 1.0f);
        if (hover > 0.0f && lit < 0.5f) d->AddRectFilled(ra, rb, C(p.text, 0.05f * hover), S(12.0f));
        const ImVec4 col = Mix(p.text, p.accent, lit * 0.8f);
        const ImVec2 ic(ra.x + S(22.0f), (ra.y + rb.y) * 0.5f);
        if (entries[i].icon == nullptr) {
            DrawSpotifyLogo(d, ic, S(18.0f), g_fade);
        } else {
            icons::Draw(d, entries[i].icon, ic, S(17.0f), C(col, 0.75f + 0.25f * std::max(lit, hover)), S(1.7f));
        }
        Text(d, g_fonts.semibold, ImVec2(ra.x + S(42.0f), (ra.y + rb.y - g_fonts.semibold->FontSize) * 0.5f), C(col), T(entries[i].key));
    }
    return changed;
}

void DrawSettingsSection(ImDrawList* d, ImVec2 min, ImVec2 max, const account::Info& acc, const LocalData& data) {
    // --- the index (left) ---
    const IndexEntry entries[] = {{kIconSliders, "settings.general"}, {icons::kMusic, "set.music"},  {nullptr, "nav.spotify"},
                                  {icons::kPerson, "settings.account"}, {icons::kGamepad, "set.game"}, {icons::kInfo, "settings.about"},
                                  {icons::kTrophy, "year.tab"}};
    const float navW = S(230.0f);
    const ImVec2 na = min, nb(min.x + navW, max.y);
    if (DrawSectionIndex(d, na, nb, T("nav.settings"), entries, 7, &g_settingsTab, &g_settingsPill, "set")) g_settingsTabIn = 0.0f;
    // --- the chosen section (right), coming in softly ---
    g_settingsTabIn = std::min(1.0f, g_settingsTabIn + Dt() * 6.0f);
    const float e = EaseOutCubic(g_settingsTabIn);
    const ImVec2 ca(nb.x + S(20.0f), min.y), cb(std::min(max.x, nb.x + S(20.0f) + S(860.0f)), max.y);
    const int start = d->VtxBuffer.Size;
    d->PushClipRect(ImVec2(ca.x - S(4.0f), ca.y - S(4.0f)), ImVec2(cb.x + S(4.0f), cb.y + S(4.0f)), true);
    switch (g_settingsTab) {
        case 0: DrawSettingsGeneral(d, ca, ImVec2(cb.x, ca.y + S(392.0f))); break;
        case 1: DrawSettingsMusic(d, ca, ImVec2(cb.x, std::min(cb.y, ca.y + S(560.0f)))); break;
        case 2: DrawSettingsSpotify(d, ca, ImVec2(cb.x, cb.y)); break;
        case 3:
            DrawSettingsAccount(d, ca, ImVec2(cb.x, ca.y + S(240.0f)), acc, data);
            DrawSettingsCloud(d, ImVec2(ca.x, ca.y + S(240.0f) + S(kGap)), ImVec2(cb.x, cb.y), acc);
            break;
        case 4: DrawSettingsGame(d, ca, cb, data); break;
        case 6: DrawSettingsYear(d, ca, cb, acc); break;
        default: DrawSettingsAboutAll(d, ca, cb); break;
    }
    d->PopClipRect();
    AnimateSince(d, start, e, ImVec2((1.0f - e) * S(24.0f), 0.0f));
}

// =============================================================================
//  THE LYRICS SECTION (design C): a whole section like Settings. The index on
//  the left; on the right, at the top, THE STAGE (your song's line in your
//  style, on a little Minecraft night) and under it the chosen part.
// =============================================================================
int g_lyricsTab = 0;  // 0 ready-made styles, 1 your styles, 2 where, 3 timing, 4 in the game, 5 the gallery
float g_lyricsTabIn = 1.0f;
float g_lyricsPill = 0.0f;

// "Where they're shown": two big choices, each with a small drawing of it.
void DrawLyricsWhere(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    const flowcfg::State& s = flowcfg::g_state;
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("lyrics.tab_where"));
    const float top = min.y + S(62.0f);
    const float gap = S(16.0f);
    const float cw = (max.x - min.x - S(40.0f) - gap) * 0.5f;
    const float ch = std::min(max.y - top - S(20.0f), S(230.0f));
    for (int i = 0; i < 2; ++i) {
        const int mode = i == 0 ? 1 : 0;  // (the sky first: it's the default)
        const ImVec2 a(min.x + S(20.0f) + (cw + gap) * static_cast<float>(i), top), b(a.x + cw, top + ch);
        char id[24];
        std::snprintf(id, sizeof(id), "##lyr_where%d", i);
        float hover = 0.0f;
        if (Hit(id, a, b, &hover) && s.mode != mode) flowcfg::Change([&](nlohmann::json& f) { f["mode"] = mode; });
        const float lit = Anim(ImGui::GetID(id) + 1, s.mode == mode, 8.0f);
        d->AddRectFilled(a, b, C(p.text, 0.04f + 0.04f * hover), S(16.0f));
        d->AddRect(a, b, C(Mix(ImVec4(1, 1, 1, 1), p.accent, lit), 0.10f + 0.9f * lit), S(16.0f), 0, S(1.0f) + S(1.0f) * lit);
        // the drawing: a horizon, and where the words go
        const ImVec2 va(a.x + S(14.0f), a.y + S(14.0f)), vb(b.x - S(14.0f), a.y + ch * 0.58f);
        d->AddRectFilledMultiColor(va, vb, C(ImVec4(0.06f, 0.07f, 0.14f, 1)), C(ImVec4(0.06f, 0.07f, 0.14f, 1)),
                                   C(ImVec4(0.18f, 0.22f, 0.36f, 1)), C(ImVec4(0.18f, 0.22f, 0.36f, 1)));
        const float ground = vb.y - (vb.y - va.y) * 0.28f;
        d->AddRectFilled(ImVec2(va.x, ground), vb, C(ImVec4(0.10f, 0.16f, 0.10f, 1)));
        const float wy = mode == 1 ? va.y + (vb.y - va.y) * 0.22f : ground - (vb.y - va.y) * 0.22f;
        const float bar = (vb.x - va.x) * (mode == 1 ? 0.55f : 0.40f);
        const ImVec2 wc((va.x + vb.x) * 0.5f, wy);
        SoftLightOval(d, wc, bar * 0.6f, S(14.0f), p.accent, 0.5f * lit + 0.15f);
        d->AddRectFilled(ImVec2(wc.x - bar * 0.5f, wy - S(3.0f)), ImVec2(wc.x + bar * 0.5f, wy + S(3.0f)), C(ImVec4(1, 1, 1, 1), 0.9f), S(3.0f));
        // the name and what it means
        float ty = vb.y + S(12.0f);
        Text(d, g_fonts.semibold, ImVec2(a.x + S(16.0f), ty), C(p.text), T(mode == 1 ? "lyrics.where_sky" : "lyrics.where_world"));
        if (lit > 0.01f) {
            const ImVec2 c(b.x - S(24.0f), ty + g_fonts.semibold->FontSize * 0.5f);
            d->AddCircleFilled(c, S(9.0f), C(p.accent, lit), 24);
            icons::Draw(d, kIconCheck, c, S(11.0f), C(p.onAccent, lit), S(2.2f));
        }
        ty += g_fonts.semibold->FontSize + S(4.0f);
        TextWrap(d, g_fonts.tiny, ImVec2(a.x + S(16.0f), ty), C(p.textDim), T(mode == 1 ? "lyrics.sky_hint" : "lyrics.world_hint"), cw - S(32.0f));
    }
}

// "Timing": the same setting as the game's Offset, big.
void DrawLyricsTiming(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("lyrics.tab_timing"));
    float y = min.y + S(56.0f);
    y += TextWrap(d, g_fonts.tiny, ImVec2(min.x + S(22.0f), y), C(p.textDim), T("lyrics.timing_hint"), max.x - min.x - S(44.0f)) + S(26.0f);
    const float off = flowcfg::g_state.offset;
    char value[24];
    std::snprintf(value, sizeof(value), "%+.1f s", off);
    const float cx = (min.x + max.x) * 0.5f;
    const ImVec2 vs = TextSize(g_fonts.hero, value);
    Text(d, g_fonts.hero, ImVec2(cx - vs.x * 0.5f, y), C(off == 0.0f ? p.text : p.accent), value);
    const float cy = y + vs.y * 0.5f;
    const float half = std::max(vs.x * 0.5f, S(70.0f)) + S(40.0f);
    if (RoundButton(d, "##lyr_off_minus", ImVec2(cx - half, cy), S(22.0f), icons::kMinus, S(16.0f), false)) {
        flowcfg::SetOffset(std::clamp(off - 0.1f, -5.0f, 5.0f));
    }
    if (RoundButton(d, "##lyr_off_plus", ImVec2(cx + half, cy), S(22.0f), icons::kPlus, S(16.0f), false)) {
        flowcfg::SetOffset(std::clamp(off + 0.1f, -5.0f, 5.0f));
    }
    y += vs.y + S(12.0f);
    const char* what = off < 0.0f ? T("lyrics.timing_earlier") : (off > 0.0f ? T("lyrics.timing_later") : T("lyrics.timing_zero"));
    const ImVec2 ws = TextSize(g_fonts.tiny, what);
    Text(d, g_fonts.tiny, ImVec2(cx - ws.x * 0.5f, y), C(p.textDim), what);
    y += ws.y + S(16.0f);
    if (off != 0.0f) {
        const char* reset = T("lyrics.timing_reset");
        const float w = TextSize(g_fonts.semibold, reset).x + S(36.0f);
        const ImVec2 a(cx - w * 0.5f, y), b(cx + w * 0.5f, y + S(34.0f));
        float hover = 0.0f;
        if (Hit("##lyr_off_reset", a, b, &hover)) flowcfg::SetOffset(0.0f);
        d->AddRectFilled(a, b, C(p.text, 0.06f + 0.08f * hover), S(17.0f));
        Text(d, g_fonts.semibold, ImVec2(a.x + S(18.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f), C(p.text), reset);
    }
}

// "In the game": is it working there, and where the rest of the options are.
void DrawLyricsGame(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data) {
    const Palette& p = P();
    const flowcfg::State& s = flowcfg::g_state;
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("lyrics.tab_game"));
    const float x = min.x + S(22.0f), w = max.x - min.x - S(44.0f);
    float y = min.y + S(64.0f);
    auto row = [&](bool ok, const char* text) {
        Dot(d, ImVec2(x + S(6.0f), y + g_fonts.semibold->FontSize * 0.5f), ok ? p.success : p.accent);
        y += TextWrap(d, g_fonts.semibold, ImVec2(x + S(20.0f), y), C(p.text), text, w - S(20.0f)) + S(14.0f);
    };
    if (s.broken) {
        Dot(d, ImVec2(x + S(6.0f), y + g_fonts.semibold->FontSize * 0.5f), p.danger);
        y += TextWrap(d, g_fonts.semibold, ImVec2(x + S(20.0f), y), C(p.text), T("lyrics.no_config"), w - S(20.0f)) + S(14.0f);
    }
    row(data.game.lumaLoaded, data.game.lumaLoaded ? T("lyrics.live") : T("lyrics.game_closed"));
    row(s.moduleOn, s.moduleOn ? T("lyrics.module_on") : T("lyrics.module_off"));
    row(s.on, s.on ? T("lyrics.shown") : T("lyrics.hidden"));
    y += S(8.0f);
    d->AddLine(ImVec2(x, y), ImVec2(x + w, y), C(p.border));
    y += S(18.0f);
    if (s.style < 0 && s.user < 0 && s.found) y += TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.text, 0.85f), T("lyrics.custom"), w) + S(10.0f);
    TextWrap(d, g_fonts.tiny, ImVec2(x, y), C(p.textDim), T("lyrics.more"), w);
}

// =============================================================================
//  THE GALLERY (Lyric Flow styles other people published, gallery.h).
//  "Explore": anyone, no account. Each card: the style's animated preview,
//  its name, who made it (or "Anonymous") and how many installed it.
//  "Install" adds it to your styles and puts it on. The "..." = report it.
//  "Yours" (signed in): publish one of your styles, and see/remove yours.
// =============================================================================
namespace galleryui {

int g_view = 0;                 // 0 explore, 1 yours
float g_scroll = 0.0f;
std::string g_reportOpen;       // the card whose report choices are open
std::string g_installed;        // the last one installed (its card says so)
int g_pick = -1;                // which of your styles is going to be published
char g_name[40] = "";
bool g_showAuthor = false;
std::string g_armedRemove;      // remove asks twice
double g_armedAt = 0.0;
int g_passVersion = -1;

void DrawExplore(ImDrawList* d, ImVec2 min, ImVec2 max, const gallery::Info& g, bool signedIn) {
    const Palette& p = P();
    if (!g.loaded && !g.loading && g.errorKey.empty()) gallery::Load(g.sort);
    // New / Popular, and "refresh"
    {
        const char* labels[2] = {T("gallery.new"), T("gallery.popular")};
        int index = static_cast<int>(g.sort);
        float sw = 0.0f;
        if (Segmented("##gal_sort", min, labels, 2, &index, &sw)) {
            g_scroll = 0.0f;
            gallery::Load(static_cast<gallery::Sort>(index));
        }
        if (IconToggle(d, "##gal_refresh", ImVec2(min.x + sw + S(26.0f), min.y + S(19.0f)), icons::kRotate, S(15.0f), false, !g.loading)) {
            gallery::Load(g.sort);
        }
    }
    const float top = min.y + S(54.0f);
    if (g.loading && g.styles.empty()) {
        Spinner(d, ImVec2((min.x + max.x) * 0.5f, top + S(60.0f)), S(9.0f), C(p.textDim));
        return;
    }
    if (!g.errorKey.empty() && g.styles.empty()) {
        TextWrap(d, g_fonts.regular, ImVec2(min.x, top + S(10.0f)), C(p.textDim), T(g.errorKey.c_str()), max.x - min.x);
        return;
    }
    if (g.styles.empty()) {
        TextWrap(d, g_fonts.regular, ImVec2(min.x, top + S(10.0f)), C(p.textDim), T("gallery.empty"), max.x - min.x);
        return;
    }
    // the cards (3 per row), scrolling with the wheel
    const float gap = S(14.0f);
    const int columns = std::max(2, std::min(4, static_cast<int>((max.x - min.x + gap) / (S(220.0f) + gap))));
    const float cw = ((max.x - min.x) - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float ch = S(196.0f);
    const int rows = (static_cast<int>(g.styles.size()) + columns - 1) / columns;
    const float moreH = g.hasMore ? S(56.0f) : 0.0f;
    const float contentH = rows * ch + std::max(0, rows - 1) * gap + moreH;
    const float maxScroll = std::max(0.0f, contentH - (max.y - top));
    if (ImGui::IsMouseHoveringRect(ImVec2(min.x, top), max)) g_scroll -= ImGui::GetIO().MouseWheel * S(90.0f);
    g_scroll = std::clamp(g_scroll, 0.0f, maxScroll);
    // Clip the cards (what they draw AND where they can be clicked). ImGui's
    // pop doesn't give the window back its own clip (it takes the draw list's),
    // so it's saved and put back by hand: otherwise nothing outside the
    // gallery (the dock, minimize, close...) could be clicked.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImRect windowClip = window->ClipRect;
    ImGui::PushClipRect(ImVec2(min.x - S(4.0f), top), ImVec2(max.x + S(4.0f), max.y), true);
    for (std::size_t i = 0; i < g.styles.size(); ++i) {
        const gallery::Style& s = g.styles[i];
        const int col = static_cast<int>(i) % columns, row = static_cast<int>(i) / columns;
        const ImVec2 a(min.x + col * (cw + gap), top + row * (ch + gap) - g_scroll), b(a.x + cw, a.y + ch);
        if (b.y < top || a.y > max.y) continue;
        Card(d, a, b);
        // the preview
        const float infoH = S(84.0f);
        const ImVec2 va(a.x + S(10.0f), a.y + S(10.0f)), vb(b.x - S(10.0f), b.y - infoH);
        d->AddRectFilled(va, vb, C(ImVec4(0.03f, 0.03f, 0.05f, 1.0f), 0.55f), S(kRound) - S(4.0f));
        d->PushClipRect(va, vb, true);
        DrawFlowLook(d, ImVec2((va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f), vb.y - va.y, LookFromJson(s.look), g_fade,
                     static_cast<float>(i) * 0.41f, "LUMA", -1.0f, 0.0f);
        d->PopClipRect();
        // name, author, installs
        const float tx = a.x + S(14.0f);
        float ty = vb.y + S(8.0f);
        TextFit(d, g_fonts.semibold, ImVec2(tx, ty), C(p.text), s.name.c_str(), b.x - tx - S(40.0f));
        ty += g_fonts.semibold->FontSize + S(2.0f);
        char count[64];
        std::snprintf(count, sizeof(count), T(s.installs == 1 ? "gallery.install_one" : "gallery.installs"), s.installs);
        char sub[160];
        std::snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", s.author.empty() ? T("gallery.anonymous") : s.author.c_str(), count);
        TextFit(d, g_fonts.tiny, ImVec2(tx, ty), C(p.textDim), sub, b.x - tx - S(10.0f));
        // "..." -> report
        char id[48];
        std::snprintf(id, sizeof(id), "##gal_more_%s", s.id.c_str());
        if (IconToggle(d, id, ImVec2(b.x - S(24.0f), vb.y + S(8.0f) + g_fonts.semibold->FontSize * 0.5f), icons::kEllipsis, S(16.0f),
                       g_reportOpen == s.id)) {
            g_reportOpen = g_reportOpen == s.id ? std::string() : s.id;
        }
        const float by = b.y - S(44.0f);
        if (g_reportOpen == s.id) {
            // the reasons (or "sign in first")
            if (!signedIn) {
                TextFit(d, g_fonts.tiny, ImVec2(tx, by + S(10.0f)), C(p.warning), T("gallery.report_sign_in"), b.x - tx - S(10.0f));
                continue;
            }
            const char* reasons[3][2] = {{"offensive_name", "gallery.reason_name"}, {"spam", "gallery.reason_spam"}, {"other", "gallery.reason_other"}};
            float x = tx;
            for (const auto& r : reasons) {
                std::snprintf(id, sizeof(id), "##gal_rep_%s_%s", r[0], s.id.c_str());
                float w = 0.0f;
                if (SmallPill(d, id, ImVec2(x, by + S(2.0f)), nullptr, T(r[1]), false, &w)) {
                    gallery::Report(s.id, r[0]);
                    g_reportOpen.clear();
                }
                x += w + S(6.0f);
            }
            continue;
        }
        std::snprintf(id, sizeof(id), "##gal_inst_%s", s.id.c_str());
        const bool installed = g_installed == s.id;
        if (Button(id, ImVec2(tx, by - S(2.0f)), T(installed ? "gallery.installed" : "gallery.install"), !installed)) {
            flowcfg::InstallGalleryStyle(s.name, gallery::LookOnly(s.look));
            gallery::CountInstall(s.id);
            g_installed = s.id;
        }
    }
    if (g.hasMore) {
        const float y = top + rows * ch + std::max(0, rows - 1) * gap + S(12.0f) - g_scroll;
        if (Button("##gal_more", ImVec2(min.x, y), T("gallery.more"), false, nullptr, !g.loading)) gallery::Load(g.sort, true);
    }
    ImGui::PopClipRect();
    window->ClipRect = windowClip;
}

void DrawYours(ImDrawList* d, ImVec2 min, ImVec2 max, const gallery::Info& g) {
    const Palette& p = P();
    if (!g.mineLoaded && !g.mineLoading) gallery::LoadMine();
    const float w = max.x - min.x;
    const float leftW = std::min(w * 0.55f, S(520.0f));
    float y = min.y;
    // --- publish (left) ---
    Text(d, g_fonts.semibold, ImVec2(min.x, y), C(p.text), T("gallery.publish_title"));
    y += g_fonts.semibold->FontSize + S(4.0f);
    if (g.banned) {
        TextWrap(d, g_fonts.regular, ImVec2(min.x, y), C(p.danger), T("gallery.err_banned"), leftW);
    } else {
        y += TextWrap(d, g_fonts.tiny, ImVec2(min.x, y), C(p.textDim), T("gallery.publish_hint"), leftW) + S(10.0f);
        // which of your styles
        const auto& list = flowcfg::g_presets;
        float x = min.x;
        int count = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].autoSaved) continue;
            ++count;
            char id[32];
            std::snprintf(id, sizeof(id), "##gal_pick%zu", i);
            const float pw = TextSize(g_fonts.semibold, list[i].name.c_str()).x + S(28.0f);
            if (x + pw > min.x + leftW && x > min.x) {
                x = min.x;
                y += S(40.0f);
            }
            const bool on = g_pick == static_cast<int>(i);
            float bw = 0.0f;
            if (SmallPill(d, id, ImVec2(x, y), nullptr, list[i].name.c_str(), false, &bw)) {
                g_pick = static_cast<int>(i);
                std::snprintf(g_name, sizeof(g_name), "%s", list[i].name.c_str());
            }
            if (on) d->AddRect(ImVec2(x, y), ImVec2(x + bw, y + S(34.0f)), C(p.accent), S(17.0f), 0, S(2.0f));
            x += bw + S(6.0f);
        }
        if (count == 0) {
            TextWrap(d, g_fonts.regular, ImVec2(min.x, y), C(p.textDim), T("gallery.no_styles"), leftW);
            y += S(30.0f);
        } else {
            y += S(46.0f);
        }
        if (g_pick >= static_cast<int>(list.size())) g_pick = -1;
        if (g_pick >= 0) {
            // the name
            const ImVec2 a(min.x, y), b(min.x + leftW, y + S(38.0f));
            d->AddRectFilled(a, b, C(ImVec4(0, 0, 0, 1), 0.30f), S(10.0f));
            d->AddRect(a, b, C(ImVec4(1, 1, 1, 1), 0.15f), S(10.0f));
            ImGui::SetCursorScreenPos(ImVec2(a.x + S(12.0f), (a.y + b.y - g_fonts.semibold->FontSize) * 0.5f - S(3.0f)));
            ImGui::PushFont(g_fonts.semibold);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, p.text);
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, p.textDim);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(3.0f)));
            ImGui::SetNextItemWidth(leftW - S(24.0f));
            ImGui::InputTextWithHint("##gal_name", T("gallery.name_hint"), g_name, sizeof(g_name));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            ImGui::PopFont();
            y += S(50.0f);
            // show my gamertag
            Toggle("##gal_author", ImVec2(min.x, y), &g_showAuthor);
            TextFit(d, g_fonts.regular, ImVec2(min.x + S(56.0f), y + S(3.0f)), C(p.text), T("gallery.show_author"), leftW - S(56.0f));
            y += S(40.0f);
            std::string name = g_name;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (Button("##gal_publish", ImVec2(min.x, y), T("gallery.publish"), true, nullptr, !g.busy && !name.empty())) {
                gallery::Publish(name, list[static_cast<std::size_t>(g_pick)].look, g_showAuthor);
            }
            y += S(46.0f);
        }
    }
    // --- the result of the last action ---
    if (g.busy) {
        Spinner(d, ImVec2(min.x + S(7.0f), y + S(9.0f)), S(5.5f), C(p.textDim));
    } else if (!g.actionKey.empty()) {
        Dot(d, ImVec2(min.x + S(7.0f), y + S(9.0f)), g.actionOk ? p.success : p.danger);
        TextWrap(d, g_fonts.regular, ImVec2(min.x + S(20.0f), y), C(p.text), T(g.actionKey.c_str()), leftW - S(20.0f));
    }
    // --- your published styles (right) ---
    const float rx = min.x + leftW + S(28.0f);
    if (rx > max.x - S(160.0f)) return;
    float ry = min.y;
    Text(d, g_fonts.semibold, ImVec2(rx, ry), C(p.text), T("gallery.yours_title"));
    ry += g_fonts.semibold->FontSize + S(10.0f);
    if (g.mine.empty()) {
        TextWrap(d, g_fonts.tiny, ImVec2(rx, ry), C(p.textDim), T(g.mineLoaded ? "gallery.none_yet" : "gallery.loading"), max.x - rx);
        return;
    }
    if (ImGui::GetTime() - g_armedAt > 4.0) g_armedRemove.clear();
    for (const gallery::Style& s : g.mine) {
        if (ry + S(44.0f) > max.y) break;
        const ImVec2 a(rx, ry), b(max.x, ry + S(48.0f));
        d->AddRectFilled(a, b, C(p.text, 0.04f), S(10.0f));
        TextFit(d, g_fonts.semibold, ImVec2(a.x + S(12.0f), a.y + S(6.0f)), C(p.text), s.name.c_str(), b.x - a.x - S(140.0f));
        char count[64];
        std::snprintf(count, sizeof(count), T(s.installs == 1 ? "gallery.install_one" : "gallery.installs"), s.installs);
        char sub[160];
        std::snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", T(s.status == "visible" ? "gallery.mine_visible" : "gallery.mine_hidden"), count);
        TextFit(d, g_fonts.tiny, ImVec2(a.x + S(12.0f), a.y + S(8.0f) + g_fonts.semibold->FontSize), C(s.status == "visible" ? p.textDim : p.warning),
                sub, b.x - a.x - S(140.0f));
        char id[48];
        std::snprintf(id, sizeof(id), "##gal_rm_%s", s.id.c_str());
        const bool armed = g_armedRemove == s.id;
        const char* label = T(armed ? "gallery.remove_confirm" : "gallery.remove");
        const float pw = TextSize(g_fonts.semibold, label).x + S(44.0f);
        if (SmallPill(d, id, ImVec2(b.x - pw - S(7.0f), a.y + S(7.0f)), icons::kTrash, label, false)) {
            if (armed) {
                gallery::Unpublish(s.id);
                g_armedRemove.clear();
            } else {
                g_armedRemove = s.id;
                g_armedAt = ImGui::GetTime();
            }
        }
        ry += S(56.0f);
    }
}

}  // namespace galleryui

void DrawLyricsGallery(ImDrawList* d, ImVec2 min, ImVec2 max) {
    const Palette& p = P();
    const account::Info acc = account::Get();
    const bool signedIn = acc.state == account::State::SignedIn && !account::LumaPass().empty();
    if (account::LumaPassVersion() != galleryui::g_passVersion) {  // signed in/out: "yours" is someone else's now
        galleryui::g_passVersion = account::LumaPassVersion();
        gallery::Reset();
    }
    const gallery::Info g = gallery::Get();
    Card(d, min, max);
    SectionTitle(d, ImVec2(min.x + S(20.0f), min.y + S(18.0f)), T("lyrics.tab_gallery"));
    // Explore / Yours
    {
        const char* labels[2] = {T("gallery.explore"), T("gallery.yours")};
        const float w = SegmentedWidth(labels, signedIn ? 2 : 1);
        if (!signedIn) galleryui::g_view = 0;
        Segmented("##gal_view", ImVec2(max.x - S(20.0f) - w, min.y + S(12.0f)), labels, signedIn ? 2 : 1, &galleryui::g_view);
    }
    const ImVec2 a(min.x + S(20.0f), min.y + S(62.0f)), b(max.x - S(20.0f), max.y - S(16.0f));
    if (galleryui::g_view == 1 && signedIn) {
        galleryui::DrawYours(d, a, b, g);
    } else {
        galleryui::DrawExplore(d, a, b, g, signedIn);
        // a report/remove result (shown here too: reporting happens in Explore)
        if (!g.actionKey.empty() && !g.busy) {
            const float x = min.x + S(20.0f) + S(320.0f);
            TextFit(d, g_fonts.tiny, ImVec2(x, min.y + S(74.0f)), C(g.actionOk ? p.success : p.danger), T(g.actionKey.c_str()),
                    max.x - S(20.0f) - x);
        }
    }
}

void DrawLyricsSection(ImDrawList* d, ImVec2 min, ImVec2 max, const LocalData& data) {
    const Palette& p = P();
    flowcfg::Refresh();
    const IndexEntry entries[] = {{icons::kSparkle, "lyrics.tab_ready"}, {icons::kLayers, "lyrics.tab_yours"},
                                  {icons::kMapPin, "lyrics.tab_where"},  {icons::kTimer, "lyrics.tab_timing"},
                                  {icons::kGamepad, "lyrics.tab_game"},  {icons::kCompass, "lyrics.tab_gallery"}};
    const float navW = S(230.0f);
    const ImVec2 na = min, nb(min.x + navW, max.y);
    if (DrawSectionIndex(d, na, nb, T("nav.lyrics"), entries, 6, &g_lyricsTab, &g_lyricsPill, "lyr")) g_lyricsTabIn = 0.0f;
    // --- the right: the stage, and the chosen part under it ---
    const media::Track t = media::Current();
    const ImVec2 ca(nb.x + S(20.0f), min.y), cb(std::min(max.x, nb.x + S(20.0f) + S(980.0f)), max.y);
    const float stageH = std::clamp((cb.y - ca.y) * 0.34f, S(150.0f), S(230.0f));
    DrawLyricsStage(d, ca, ImVec2(cb.x, ca.y + stageH), t);
    g_lyricsTabIn = std::min(1.0f, g_lyricsTabIn + Dt() * 6.0f);
    const float e = EaseOutCubic(g_lyricsTabIn);
    const ImVec2 pa(ca.x, ca.y + stageH + S(kGap)), pb = cb;
    const int start = d->VtxBuffer.Size;
    d->PushClipRect(ImVec2(pa.x - S(4.0f), pa.y - S(4.0f)), ImVec2(pb.x + S(4.0f), pb.y + S(4.0f)), true);
    switch (g_lyricsTab) {
        case 0: DrawStyleCards(d, pa, pb, g_lyricsTabIn); break;
        case 1: {
            Card(d, pa, pb);
            SectionTitle(d, ImVec2(pa.x + S(20.0f), pa.y + S(18.0f)), T("lyrics.your_styles"));
            DrawUserStyles(d, ImVec2(pa.x + S(20.0f), pa.y), ImVec2(pb.x - S(20.0f), pb.y), pa.y + S(62.0f));
            break;
        }
        case 2: DrawLyricsWhere(d, pa, ImVec2(pb.x, std::min(pb.y, pa.y + S(340.0f)))); break;
        case 3: DrawLyricsTiming(d, pa, ImVec2(pb.x, std::min(pb.y, pa.y + S(300.0f)))); break;
        case 5: DrawLyricsGallery(d, pa, pb); break;
        default: DrawLyricsGame(d, pa, ImVec2(pb.x, std::min(pb.y, pa.y + S(320.0f))), data); break;
    }
    d->PopClipRect();
    AnimateSince(d, start, e, ImVec2((1.0f - e) * S(24.0f), 0.0f));
    (void)p;
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
        static const bool pretendGame = std::wcsstr(GetCommandLineW(), L"--year-pretend-game") != nullptr;  // (for testing)
        listening::SetGameRunning(data.game.running || pretendGame);
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

void BuildFirstFonts();  // (the fonts: below, with their helper thread)
bool FontJobDone();

void AppInit(HWND hwnd, float scale) {
    luma::launcher::smtc::Init(hwnd);  // (Luma's own player tells Windows what plays)
    perf::Init();  // (the music's clock: core/media.cpp)
    // HD covers: 1024 px (the big cover fills more than 512 on a scaled
    // screen); Apple Music's 1000 px when the app gives a small one (the same
    // as the game's option); Spotify's own 640 px when it plays from Spotify
    // (UpdateHdArt).
    media::SetArtSize(1024);
    lyrics::SetHdCovers(true);
    g_hwnd = hwnd;
    g_scale = scale;
    LoadConfig();
    g_startWithWindows = ReadStartup();
    if (g_startWithWindows) {
        WriteStartup(true);  // por si moviste el launcher a otra carpeta: actualizar la ruta
    }
    BuildFirstFonts();
    skins::Load();
    // "--page=skins" / "--page=settings": abrir directo en esa sección (para pruebas).
    if (std::wcsstr(GetCommandLineW(), L"--page=skins") != nullptr) g_page = Page::Skins;
    if (std::wcsstr(GetCommandLineW(), L"--page=settings") != nullptr) g_page = Page::Settings;
    if (std::wcsstr(GetCommandLineW(), L"--page=lyrics") != nullptr) g_page = Page::Lyrics;
    if (std::wcsstr(GetCommandLineW(), L"--page=spotify") != nullptr) g_page = Page::Spotify;
    if (std::wcsstr(GetCommandLineW(), L"--view=lyrics") != nullptr) g_openViewWhenReady = true;
    if (std::wcsstr(GetCommandLineW(), L"--luma-player") != nullptr) luma::launcher::spotifyengine::Start();  // (for testing)
    if (std::wcsstr(GetCommandLineW(), L"--sp-devices") != nullptr) sp::g_drawerAsk = 1;  // (for testing: the Devices drawer)
    if (std::wcsstr(GetCommandLineW(), L"--sp-search") != nullptr) sp::OpenSearch();  // (for testing: the search, open)
    if (const wchar_t* pl = std::wcsstr(GetCommandLineW(), L"--sp-playlist=")) {  // (for testing: open a playlist's page)
        std::wstring id(pl + 14);
        id = id.substr(0, id.find_first_of(L" \""));
        sp::Item it;
        it.type = sp::kPlaylist;
        it.id = std::string(id.begin(), id.end());
        it.uri = "spotify:playlist:" + it.id;
        sp::Open(it);
    }
    if (const wchar_t* rs = std::wcsstr(GetCommandLineW(), L"--sp-radio=")) {  // (for testing: Spotify's radio of a URI)
        std::wstring u(rs + 11);
        u = u.substr(0, u.find_first_of(L" \""));
        sp::Item it;
        it.type = sp::kTrack;
        it.uri = std::string(u.begin(), u.end());
        it.name = "test";
        sp::StartRadio(it);
    }
    if (const wchar_t* lt = std::wcsstr(GetCommandLineW(), L"--lyr-tab=")) g_lyricsTab = std::clamp(static_cast<int>(lt[10] - L'0'), 0, 5);  // (for testing)
    if (const wchar_t* st = std::wcsstr(GetCommandLineW(), L"--set-tab=")) g_settingsTab = std::clamp(static_cast<int>(st[10] - L'0'), 0, 6);  // (for testing)
    if (std::wcsstr(GetCommandLineW(), L"--luma-play") != nullptr) sp::WithLumaDevice([](const std::string& id) { sp::ContinueOn(id); });  // (for testing)
    g_statusThread = std::thread(StatusThread);
    account::Init();
    cloudsync::Start();  // "save my styles in my account" (does nothing while it's off)
    listening::Start();  // "your year in Luma": what plays while Minecraft is open
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

void RequestFontsRebuild() {
    g_fontsDirty = true;
}

bool AppFontsDirty() {
    return FontJobDone();  // (a new atlas is ready: window.cpp swaps it in)
}

// Every font of the launcher, into "atlas". It runs in the fonts' helper
// thread (or at the start), so it only touches what it is given.
Fonts BuildFonts(ImFontAtlas* atlas, float scale, std::string& pixelDir) {
    Fonts fonts;
    atlas->Clear();
    atlas->TexGlyphPadding = kAtlasPadding;  // (room for the blurred copies: BlurText)
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
        return atlas->AddFontFromFileTTF(path.string().c_str(), size * scale, &config);
    };
    // (song titles and lyrics in other alphabets: their letters are merged
    // into the fonts that show them: render/font_fallback.h)
    render::fontfallback::BeginBuild();
    auto merge = [&](ImFont* font, float size) {
        if (font != nullptr) render::fontfallback::MergeInto(atlas, size * scale);
    };
    fonts.regular = load(L"segoeui.ttf", 15.0f);
    if (fonts.regular == nullptr) {
        fonts.regular = atlas->AddFontDefault();
    }
    merge(fonts.regular, 15.0f);
    fonts.tiny = load(L"segoeui.ttf", 13.0f);
    fonts.semibold = load(L"seguisb.ttf", 16.0f);
    merge(fonts.semibold, 16.0f);
    fonts.title = load(L"segoeuib.ttf", 22.0f);
    fonts.hero = load(L"segoeuib.ttf", 42.0f);
    merge(fonts.hero, 42.0f);
    fonts.lyric = load(L"segoeuib.ttf", 34.0f);
    merge(fonts.lyric, 34.0f);
    fonts.line = load(L"seguisb.ttf", 21.0f);
    merge(fonts.line, 21.0f);
    // La letra pixelada: no la distribuimos; se lee de la carpeta de TU juego
    // (Luma.dll anota cuál es en profile.json).
    fonts.pixel = nullptr;
    {
        const nlohmann::json profile = ReadJson(DataDir() / L"profile.json");
        pixelDir = profile.value("gameDir", std::string());
        if (!pixelDir.empty()) {
            const fs::path font = fs::path(std::u8string(reinterpret_cast<const char8_t*>(pixelDir.data()), pixelDir.size())) / L"data" / L"fonts" / L"Mojangles.ttf";
            std::error_code ec;
            if (fs::exists(font, ec)) {
                ImFontConfig config;
                config.OversampleH = 1;
                config.OversampleV = 1;
                config.PixelSnapH = true;
                fonts.pixel = atlas->AddFontFromFileTTF(font.string().c_str(), std::round(20.0f * scale), &config);
            }
        }
    }
    ImFont* fallback = fonts.regular;
    if (fonts.tiny == nullptr) fonts.tiny = fallback;
    if (fonts.semibold == nullptr) fonts.semibold = fallback;
    if (fonts.title == nullptr) fonts.title = fonts.semibold;
    if (fonts.hero == nullptr) fonts.hero = fonts.title;
    if (fonts.lyric == nullptr) fonts.lyric = fonts.hero;
    if (fonts.line == nullptr) fonts.line = fonts.title;
    if (fonts.pixel == nullptr) fonts.pixel = fonts.semibold;
    atlas->Build();
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &w, &h);  // (ready for the GPU: no work left for the UI thread)
    return fonts;
}

// --- THE FONTS ARE BUILT IN THE BACKGROUND ------------------------------------------
// New letters (a Japanese song name in the Spotify section, say) need a new
// font atlas: half a second of reading font files, and its blurred copies, two
// seconds more. Done on the UI thread, the window froze for seconds. So a
// helper thread builds a whole new atlas (and its blur); the UI keeps drawing
// with the old one and changes to the new one in a single moment, between two
// frames (window.cpp). Meanwhile the new letters just show a moment later.
struct FontJob {
    ImFontAtlas* atlas = nullptr;  // the new atlas (nullptr: only the blur of the current one)
    Fonts fonts;
    std::string pixelDir;
    std::vector<unsigned char> alpha;  // (blur only: a copy of the current atlas' letters)
    int w = 0, h = 0;
    // (already on the GPU: Direct3D 11 lets other threads create textures, so
    // the UI thread only swaps pointers; sending 16 MB pictures took ~50 ms)
    ID3D11ShaderResourceView* fontView = nullptr;
    ID3D11ShaderResourceView* blurView[2] = {nullptr, nullptr};
};
std::unique_ptr<FontJob> g_fontJob;
std::thread g_fontThread;
std::atomic<bool> g_fontJobDone{false};
ID3D11ShaderResourceView* g_fontView = nullptr;  // the letters' picture on the GPU (ours, after the first)
ImFontAtlas* g_oldAtlas = nullptr;                // the one before: the next helper frees it (~50 ms)

// An RGBA picture to the GPU (any thread). nullptr if it fails.
ID3D11ShaderResourceView* MakeTexture(const void* rgba, int w, int h) {
    if (g_device == nullptr || rgba == nullptr || w <= 0 || h <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{rgba, static_cast<UINT>(w * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture))) {
        g_device->CreateShaderResourceView(texture, nullptr, &view);
        texture->Release();
    }
    return view;
}

void RunFontJob(FontJob* job, ImFontAtlas* trash, float scale) {
    if (trash != nullptr) IM_DELETE(trash);  // (an atlas nobody uses any more)
    const unsigned char* alpha = job->alpha.data();
    if (job->atlas != nullptr) {
        job->fonts = BuildFonts(job->atlas, scale, job->pixelDir);
        unsigned char* pixels = nullptr;
        job->atlas->GetTexDataAsRGBA32(&pixels, &job->w, &job->h);
        job->fontView = MakeTexture(pixels, job->w, job->h);
        unsigned char* a8 = nullptr;
        job->atlas->GetTexDataAsAlpha8(&a8, &job->w, &job->h);
        alpha = a8;
    }
    if (alpha != nullptr && job->w > 0) {
        std::vector<std::uint8_t> blur[2];
        BlurLetters(alpha, job->w, job->h, blur);
        for (int level = 0; level < 2; ++level) job->blurView[level] = MakeTexture(blur[level].data(), job->w, job->h);
    }
    g_fontJobDone.store(true);
}

bool FontJobDone() {
    return g_fontJobDone.load();
}

void StartFontJob(bool blurOnly) {
    if (g_fontThread.joinable()) return;  // (one at a time; the next one after this)
    auto job = std::make_unique<FontJob>();
    if (blurOnly) {
        unsigned char* alpha = nullptr;
        ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&alpha, &job->w, &job->h);
        if (alpha == nullptr) return;
        job->alpha.assign(alpha, alpha + static_cast<std::size_t>(job->w) * job->h);
    } else {
        g_fontsDirty = false;
        render::fontfallback::BeginBuild();  // (the needed letters, fixed for this build: UI thread)
        job->atlas = IM_NEW(ImFontAtlas)();
    }
    g_fontJobDone.store(false);
    FontJob* raw = job.get();
    g_fontJob = std::move(job);
    ImFontAtlas* trash = g_oldAtlas;
    g_oldAtlas = nullptr;
    g_fontThread = std::thread(RunFontJob, raw, trash, g_scale);
}

void ReleaseFontJob(FontJob& job) {
    if (job.atlas != nullptr) IM_DELETE(job.atlas);
    job.atlas = nullptr;
    if (job.fontView != nullptr) job.fontView->Release();
    job.fontView = nullptr;
    for (auto*& v : job.blurView) {
        if (v != nullptr) v->Release();
        v = nullptr;
    }
}

void StopFontJob() {
    if (g_fontThread.joinable()) g_fontThread.join();
    if (g_fontJob) ReleaseFontJob(*g_fontJob);
    g_fontJob.reset();
    if (g_oldAtlas != nullptr) IM_DELETE(g_oldAtlas);
    g_oldAtlas = nullptr;
    if (g_fontView != nullptr) g_fontView->Release();
    g_fontView = nullptr;
}

// The first fonts, at the start (there is nothing to draw with yet): built
// right here; only their blur goes to the helper thread.
void BuildFirstFonts() {
    g_fontsDirty = false;
    render::fontfallback::BeginBuild();
    g_fonts = BuildFonts(ImGui::GetIO().Fonts, g_scale, g_pixelFontDir);
    StartFontJob(true);
}

void AppRebuildFonts() {
    // (window.cpp calls this between two frames, when AppFontsDirty says a
    // finished build is waiting: the new atlas takes the old one's place, and
    // that is only a few pointers)
    if (!g_fontJobDone.load()) return;
    g_fontThread.join();
    std::unique_ptr<FontJob> job = std::move(g_fontJob);
    g_fontJobDone.store(false);
    if (job->atlas != nullptr) {
        if (job->fontView == nullptr) {  // (the GPU said no: keep the fonts we have)
            ReleaseFontJob(*job);
            return;
        }
        ImGuiIO& io = ImGui::GetIO();
        if (g_oldAtlas != nullptr) IM_DELETE(g_oldAtlas);  // (rare: two changes with no job between)
        g_oldAtlas = io.Fonts;
        io.Fonts = job->atlas;
        job->atlas = nullptr;
        g_fonts = job->fonts;
        g_pixelFontDir = job->pixelDir;
        if (g_fontView != nullptr) g_fontView->Release();  // (the backend keeps its first one until the end)
        g_fontView = job->fontView;
        job->fontView = nullptr;
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(g_fontView));
    }
    ReleaseBlurAtlas();
    for (int level = 0; level < 2; ++level) {
        g_blurAtlas[level] = job->blurView[level];
        job->blurView[level] = nullptr;
    }
}

void AppFrame() {
    if (g_fontsDirty) StartFontJob(false);  // (new letters or a new size: in the background)
    TickTheme(Dt());  // (the soft change between dark and light)
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
    if (cloudsync::TakeLauncherReload()) LoadConfig();  // theme/language/sky brought from your account
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
    UpdateHdArt(track);
    flowcfg::Refresh();  // (the lyrics' offset, the style chosen in the game)
    SyncSpotifyClientId();  // (the Spotify connection, shared with the game)
    {
        // Spotify's player is asked about only while it's shown (its page, or
        // Home while Spotify plays: for Home's shuffle, repeat and heart).
        const bool connected = SpotifyConnected();
        const bool onHome = g_page == Page::Home && track.present && IsSpotifyApp(track.app);
        // (Luma's own player playing: the player is followed even when hidden,
        // to tell Windows what plays: smtc_bridge.h)
        namespace eng = luma::launcher::spotifyengine;
        const spotify::Player spl = spotify::GetPlayer();
        const bool lumaPlays = connected && eng::GetState() == eng::State::Ready && spl.hasDevice && spl.device.id == eng::DeviceId();
        luma::launcher::smtc::Update(lumaPlays);
        spotify::SetPlayerWanted((visible && connected && (g_page == Page::Spotify || onHome)) || lumaPlays);
        spotify::SetActive(visible && connected);
    }
    UpdateLyrics(track, visible);

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
    luma::launcher::spotifyengine::Stop();  // (closes Luma's own player, Soloist)
    luma::launcher::smtc::Shutdown();
    spotify::Shutdown();
    StopFontJob();
    if (g_mipSampler != nullptr) g_mipSampler->Release();
    g_mipSampler = nullptr;
    ReleaseBlurAtlas();
    lyrics::Shutdown();
    audioviz::Shutdown();
    media::Shutdown();
    ReleaseArt(g_art);
    ReleaseArt(g_artOld);
    ReleaseArt(g_blur);
    ReleaseArt(g_blobs);
    ReleaseArt(g_blobsOld);
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
    cloudsync::Stop();
    gallery::Stop();
    listening::Stop();
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

// The fonts are rebuilt when a song brings letters of another alphabet
// (render/font_fallback.cpp asks for it with this, like in the game).
namespace luma::theme {
void RequestFontRebuild() {
    luma::launcher::gui::RequestFontsRebuild();
}
}  // namespace luma::theme
