// =============================================================================
//  player_list.cpp - La lista de jugadores (ver player_list.h)
// =============================================================================
#include "modules/hud/player_list.h"

#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "core/cloud.h"
#include "core/keys.h"
#include "core/logger.h"
#include "i18n.h"
#include "perf/timer.h"
#include "render/assets.h"
#include "render/hud.h"
#include "render/input.h"
#include "render/ui.h"
#include "sdk/memory.h"
#include "sdk/mouse_grab.h"
#include "sdk/network.h"
#include "sdk/offsets.h"
#include "sdk/player.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace ui = render::ui;
using i18n::T;

PlayerList::PlayerList()
    : Module("player_list", "Player list",
             "Hold Tab to see the players with their faces, the server and your ping, with the Luma logo on those who use it.",
             Category::Hud) {}

const char* PlayerList::IconSvg() const {
    // Tres renglones con una cabecita cuadrada cada uno.
    return "M4 5h3v3H4z M10 6.5h10 M4 10.5h3v3H4z M10 12h10 M4 16h3v3H4z M10 17.5h10";
}

void PlayerList::Refresh() {
    lastRefresh_ = perf::Now();
    // Caras solo de los nuevos (o si cambió su skin): leer una cara copia algo
    // de memoria; hacerlo para todos cada segundo sería un desperdicio.
    std::vector<sdk::players::Player> list;
    if (!sdk::players::Read(list, false)) {
        players_.clear();
        if (!loggedFail_) {
            loggedFail_ = true;
            log::Warn("Lista de jugadores: no se pudo leer la lista del juego (¿cambió la versión?).");
        }
        return;
    }
    std::vector<std::string> names;
    names.reserve(list.size());
    for (sdk::players::Player& p : list) {
        names.push_back(p.name);
        auto it = faces_.find(p.name);
        if (it == faces_.end() || it->second.skinId != p.skinId) {
            std::vector<sdk::players::Player> withFace;
            // (releer la lista con caras es más simple que buscar a uno; se hace pocas veces)
            if (sdk::players::Read(withFace, true)) {
                for (sdk::players::Player& f : withFace) faces_[f.name] = std::move(f);
            }
            break;
        }
    }
    players_ = std::move(list);
    char name[64]{};
    if (const std::uintptr_t player = sdk::player::LocalPlayer();
        player != 0 && sdk::SafeReadString(player + sdk::offsets::kPlayerName, name, sizeof(name))) {
        ownName_ = name;
    }
    // Orden: tú primero, después (opcional) los de Luma, después por nombre.
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string own = lower(ownName_);
    struct Sorted {
        sdk::players::Player p;
        std::string key;
        bool own;
        bool luma;
    };
    std::vector<Sorted> sorted;
    sorted.reserve(players_.size());
    for (auto& p : players_) {
        Sorted e{std::move(p), {}, false, false};
        e.key = lower(e.p.name);
        e.own = e.key == own;
        e.luma = (e.own && cloud::SignedIn()) || cloud::IsLumaUser(e.p.name);
        sorted.push_back(std::move(e));
    }
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Sorted& a, const Sorted& b) {
        if (a.own != b.own) return a.own;
        if (lumaFirst_ && a.luma != b.luma) return a.luma;
        return a.key < b.key;
    });
    players_.clear();
    rowOwn_.clear();
    rowLuma_.clear();
    for (Sorted& e : sorted) {
        rowOwn_.push_back(e.own);
        rowLuma_.push_back(e.luma);
        players_.push_back(std::move(e.p));
    }
    cloud::RequestLookup(names);  // ¿quiénes usan Luma? (el servidor responde en segundo plano)
    // ¿Servidor o mundo local? Lo sabemos por el ping (en tu propio mundo es 0).
    onServer_ = sdk::network::OnServer();
    title_.clear();
    if (onServer_) {
        if (sdk::players::ReadServer(server_)) title_ = server_.address;
    } else {
        sdk::players::WorldName(title_);
    }
    if (!logged_) {
        logged_ = true;
        log::Info("Lista de jugadores: {} jugadores; servidor: {}.", players_.size(),
                  title_.empty() ? std::string("(sin nombre)") : title_);
    }
}

ImVec2 PlayerList::OnRenderHud(const HudFrame& frame) {
    // Solo mientras mantienes la tecla, jugando (no en el chat ni en menús).
    const bool held = (GetAsyncKeyState(key_) & 0x8000) != 0 && GetForegroundWindow() == render::input::GameWindow() &&
                      sdk::mouse::GameMouseState() != 0 && !frame.editing;
    if (!held) {
        return ImVec2(0, 0);
    }
    if (lastRefresh_ == 0 || perf::TicksToMs(perf::Now() - lastRefresh_) > 1000.0) {
        Refresh();
    }
    // --- El estilo de Luma, con columnas como en Java -------------------------------
    //   Caja redondeada; arriba el mundo o servidor y "N jugadores · ping" con
    //   su color; tu fila resaltada y las demás alternadas; caras con sombra y
    //   el logo de Luma al principio. Hasta 20 por columna: con más, se
    //   reparten en columnas PAREJAS (30 jugadores = 2 columnas de 15).
    const std::size_t n = players_.size();
    const rh::Metrics m = rh::Measure(frame.scale * size_);
    const float u = m.px;  // un "píxel" del HUD
    const float rowH = std::floor(rh::LineBoxHeight(m) + 2.0f * u);
    const float face = std::floor(rowH - 5.0f * u);
    int columns = 1;
    std::size_t perColumn = n;
    while (perColumn > 20) {
        ++columns;
        perColumn = (n + columns - 1) / columns;
    }
    bool anyLuma = false;
    for (bool l : rowLuma_) anyLuma |= l;
    const float logoCol = anyLuma ? std::floor(rowH * 0.85f) : 0.0f;
    const float padX = 5.0f * u;
    float colW = 100.0f * u;
    for (const auto& p : players_) {
        colW = std::max(colW, padX * 2.0f + logoCol + (showFaces_ ? face + 4.0f * u : 0.0f) + rh::TextSize(m, p.name.c_str()).x);
    }
    const std::size_t shownRows = std::max<std::size_t>(1, perColumn);
    // Encabezado: el servidor o el mundo, y "N jugadores · ping".
    char header[160];
    std::snprintf(header, sizeof(header), "%s", !title_.empty() ? title_.c_str() : T(onServer_ ? "tab.server" : "tab.local"));
    char count[64];
    std::snprintf(count, sizeof(count), T("tab.players"), static_cast<int>(n));
    char pingText[32] = "";
    const int ping = sdk::network::PingMs();
    if (ping > 0) std::snprintf(pingText, sizeof(pingText), "%d ms", ping);
    const float gap = 4.0f * u;
    const float infoW = rh::TextSize(m, count).x + (ping > 0 ? rh::TextSize(m, pingText).x + 14.0f * u : 0.0f);
    const float headerH = showServer_ ? std::floor(rowH * 2.1f) : 0.0f;
    const float innerW = std::max(colW * columns + gap * (columns - 1), std::max(rh::TextSize(m, header).x, infoW) + 8.0f * u);
    const float width = std::floor(innerW + 8.0f * u);
    const float height = std::floor(headerH + shownRows * rowH + 8.0f * u + (showServer_ ? 3.0f * u : 0.0f));
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 min(std::floor((io.DisplaySize.x - width) * 0.5f), std::floor(io.DisplaySize.y * 0.04f));
    const ImVec2 max(min.x + width, min.y + height);
    ImDrawList* d = frame.draw;
    const rh::Style& style = rh::CurrentStyle();
    const float r = std::max(3.0f * u, style.rounding * u);
    // El fondo: el color de las cajas del HUD, un poco más opaco (es mucho texto).
    const float bgAlpha = std::clamp(style.background + 0.2f, 0.45f, 0.9f);
    d->AddRectFilled(min, max, IM_COL32(static_cast<int>(style.backgroundColor[0] * 255), static_cast<int>(style.backgroundColor[1] * 255),
                                        static_cast<int>(style.backgroundColor[2] * 255), static_cast<int>(bgAlpha * 255)), r);
    d->AddRect(min, max, rh::TextColor(0.10f), r, 0, std::max(1.0f, std::floor(u * 0.5f)));
    float y = min.y + 4.0f * u;
    if (showServer_) {
        rh::CenteredText(d, m, ImVec2(min.x, y), ImVec2(max.x, y + rowH), header, rh::TextColor());
        const float infoY = y + rowH;
        const float ix = std::floor(min.x + (width - infoW) * 0.5f);
        const ImVec2 cs = rh::TextSize(m, count);
        rh::Text(d, m, ImVec2(ix, infoY + (rowH - cs.y) * 0.5f), count, rh::TextColor(0.55f));
        if (ping > 0) {
            const ImU32 pc = ping < 80 ? IM_COL32(120, 220, 120, 255) : ping < 150 ? IM_COL32(235, 205, 90, 255) : IM_COL32(235, 110, 90, 255);
            const float dx = ix + cs.x + 7.0f * u;
            d->AddCircleFilled(ImVec2(dx, infoY + rowH * 0.5f), 2.0f * u, pc, 12);
            rh::Text(d, m, ImVec2(dx + 5.0f * u, infoY + (rowH - cs.y) * 0.5f), pingText, pc);
        }
        y += headerH;
        d->AddRectFilled(ImVec2(min.x + 4.0f * u, y), ImVec2(max.x - 4.0f * u, y + std::max(1.0f, std::floor(u * 0.5f))), rh::TextColor(0.12f));
        y += 3.0f * u;
    }
    const float listX = std::floor(min.x + (width - (colW * columns + gap * (columns - 1))) * 0.5f);
    for (std::size_t i = 0; i < n; ++i) {
        const sdk::players::Player& p = players_[i];
        const int col = static_cast<int>(i / shownRows);
        const std::size_t row = i % shownRows;
        const float x = std::floor(listX + col * (colW + gap));
        const float ry = std::floor(y + row * rowH);
        const bool isOwn = i < rowOwn_.size() && rowOwn_[i];
        const bool isLuma = i < rowLuma_.size() && rowLuma_[i];
        // Tu fila, resaltada; las demás, alternadas apenas.
        if (isOwn) {
            d->AddRectFilled(ImVec2(x, ry), ImVec2(x + colW, ry + rowH), rh::TextColor(0.10f), 2.0f * u);
        } else if (row % 2 == 1) {
            d->AddRectFilled(ImVec2(x, ry), ImVec2(x + colW, ry + rowH), rh::TextColor(0.035f), 2.0f * u);
        }
        float tx = x + padX;
        // El logo de Luma, AL PRINCIPIO de la fila.
        if (anyLuma) {
            if (isLuma) render::assets::DrawLogo(d, ImVec2(tx + logoCol * 0.42f, ry + rowH * 0.5f), rowH * 0.62f, rh::TextColor(0.95f));
            tx += logoCol;
        }
        // La cara: 8x8 cuadraditos (nítida, sin texturas), con una sombra.
        if (showFaces_) {
            auto it = faces_.find(p.name);
            const float fy = std::floor(ry + (rowH - face) * 0.5f);
            d->AddRectFilled(ImVec2(tx + u, fy + u), ImVec2(tx + face + u, fy + face + u), IM_COL32(0, 0, 0, 90));
            if (it != faces_.end() && it->second.hasFace) {
                const float cell = face / 8.0f;
                for (int py = 0; py < 8; ++py) {
                    for (int px = 0; px < 8; ++px) {
                        const std::uint8_t* c = it->second.face + (py * 8 + px) * 4;
                        d->AddRectFilled(ImVec2(std::floor(tx + px * cell), std::floor(fy + py * cell)),
                                         ImVec2(std::floor(tx + (px + 1) * cell), std::floor(fy + (py + 1) * cell)),
                                         IM_COL32(c[0], c[1], c[2], 255));
                    }
                }
            } else {
                d->AddRectFilled(ImVec2(tx, fy), ImVec2(tx + face, fy + face), rh::TextColor(0.15f));
            }
            tx += face + 4.0f * u;
        }
        const ImVec2 ts = rh::TextSize(m, p.name.c_str());
        rh::Text(d, m, ImVec2(tx, ry + (rowH - ts.y) * 0.5f), p.name.c_str(), rh::TextColor(isOwn ? 1.0f : 0.88f));
    }
    return ImVec2(width, height);
}

bool PlayerList::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Server and ping on top", &showServer_);
    changed |= ui::Toggle("Skin faces", &showFaces_);
    changed |= ui::Toggle("Luma users first", &lumaFirst_);
    changed |= ui::Slider("Size", &size_, 0.5f, 1.5f, "%.2fx");
    ui::Hint("Se abre manteniendo Tab. El logo de Luma aparece en quienes usan Luma e iniciaron sesión con Microsoft "
             "en el launcher. El ping de los demás no se puede ver: Bedrock no lo manda.");
    return changed;
}

void PlayerList::SaveSettings(nlohmann::json& out) const {
    out["key"] = key_;
    out["showServer"] = showServer_;
    out["showFaces"] = showFaces_;
    out["lumaFirst"] = lumaFirst_;
    out["size"] = size_;
}

void PlayerList::LoadSettings(const nlohmann::json& in) {
    key_ = in.value("key", 0x09);
    showServer_ = in.value("showServer", true);
    showFaces_ = in.value("showFaces", true);
    lumaFirst_ = in.value("lumaFirst", false);
    size_ = std::clamp(in.value("size", 0.85f), 0.5f, 1.5f);
}

}  // namespace luma::modules::hud
