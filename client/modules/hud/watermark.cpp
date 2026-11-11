// =============================================================================
//  watermark.cpp - Implementación de la marca de agua
// =============================================================================
#include "modules/hud/watermark.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/version.h"
#include "render/assets.h"
#include "render/hud.h"
#include "render/rainbow.h"
#include "render/music.h"
#include "render/icons.h"
#include "render/theme.h"
#include "render/ui.h"

namespace luma::modules::hud {

namespace rh = render::hud;
namespace ui = render::ui;

Watermark::Watermark()
    : Module("watermark", "Watermark", "Shows Luma's logo and name on screen.", Category::Hud) {
    // Abajo a la derecha (ancla en su esquina inferior derecha).
    HudLayout l;
    l.x = 0.996f;
    l.y = 0.992f;
    l.anchorX = 1.0f;
    l.anchorY = 1.0f;
    l.scale = 1.5f;
    SetDefaultLayout(l);
}

const char* Watermark::IconSvg() const {
    return render::icons::kLumaLogo;
}

ImVec2 Watermark::OnRenderHud(const HudFrame& frame) {
    const rh::Metrics m = rh::Measure(frame.scale);
    const float u = m.px;
    const rh::Metrics small = rh::Measure(frame.scale * 0.5f);
    char version[16] = "";
    if (showVersion_) {
        std::snprintf(version, sizeof(version), "%s", kVersion);
    }
    const ImVec2 nameSize = showName_ ? rh::TextSize(m, kName) : ImVec2(0, 0);
    const ImVec2 versionSize = showVersion_ ? rh::TextSize(small, version) : ImVec2(0, 0);
    const float pad = box_ ? 5.0f * u : 0.0f;
    const float logo = showLogo_ ? std::floor(m.capHeight * 1.5f) : 0.0f;
    const float gap = showLogo_ && showName_ ? 3.0f * u : 0.0f;
    const float width = pad * 2.0f + logo + gap + nameSize.x + (showVersion_ ? 2.0f * u + versionSize.x : 0.0f);
    const float height = std::floor(std::max(logo, m.capHeight) + pad * 2.0f + (box_ ? 0.0f : 2.0f * u));
    const ImVec2 o = frame.pos;
    if (box_) {
        rh::Box(frame.draw, m, o, ImVec2(o.x + width, o.y + height));
    }
    // El logo, en marfil (o rainbow, o el color del álbum), con la misma sombra que el texto.
    ImVec4 tint = theme::kIvory;
    if (colorMode_ == 1) {
        rainbow::Color(rainbow::kDefaultSpeed, 0.0f, tint.x, tint.y, tint.z);
    }
    if (colorMode_ == 2 || render::music::Get().watermarkAlbum) {  // also set from the Now playing module
        float album[3];
        if (render::music::AlbumColor(album)) tint = ImVec4(album[0], album[1], album[2], 1.0f);
    }
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(tint);
    const ImVec2 logoCenter(o.x + pad + logo * 0.5f, o.y + height * 0.5f);
    if (showLogo_) {
        render::assets::DrawLogo(frame.draw, ImVec2(logoCenter.x + u, logoCenter.y + u), logo, IM_COL32(0, 0, 0, 110));
        render::assets::DrawLogo(frame.draw, logoCenter, logo, color);
    }
    // "Luma" con las mayúsculas centradas respecto del logo.
    const float textX = o.x + pad + logo + gap;
    const float textY = o.y + (height - m.capHeight) * 0.5f - m.capTop;
    if (showName_) {
        rh::Text(frame.draw, m, ImVec2(textX, textY), kName, color);
    }
    if (showVersion_) {
        // La versión, chiquita, alineada abajo con el nombre.
        const float vy = textY + m.capTop + m.capHeight - small.capTop - small.capHeight;
        rh::Text(frame.draw, small, ImVec2(textX + nameSize.x + 2.0f * u, vy), version,
                 ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.67f)));
    }
    return ImVec2(std::floor(width), height);
}

bool Watermark::DrawSettings() {
    bool changed = false;
    changed |= ui::Toggle("Logo", &showLogo_);
    changed |= ui::Toggle("Name", &showName_);
    changed |= ui::Toggle("Version", &showVersion_);
    changed |= ui::Toggle("Background box", &box_);
    changed |= ui::Choice("Color", &colorMode_, {"Luma's ivory", "Rainbow", "Album colors"});
    if (colorMode_ == 2) ui::Hint("Takes the color of the song's cover (needs the Now playing module on).");
    return changed;
}

void Watermark::SaveSettings(nlohmann::json& out) const {
    out["showVersion"] = showVersion_;
    out["box"] = box_;
    out["showLogo"] = showLogo_;
    out["showName"] = showName_;
    out["colorMode"] = colorMode_;
}

void Watermark::LoadSettings(const nlohmann::json& in) {
    showVersion_ = in.value("showVersion", true);
    box_ = in.value("box", false);
    showLogo_ = in.value("showLogo", true);
    showName_ = in.value("showName", true);
    colorMode_ = std::clamp(in.value("colorMode", 0), 0, 2);
    if (!showLogo_ && !showName_) {
        showName_ = true;  // algo tiene que verse
    }
}

}  // namespace luma::modules::hud
