// =============================================================================
//  map_look.cpp - How the maps look (see map_look.h)
// =============================================================================
#include "modules/world/map_look.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace luma::modules::world::maplook {

namespace {

Look g_look;
std::uint32_t g_version = 1;
std::uint8_t g_table[256];  // brightness and contrast, per channel value
bool g_plain = true;
bool g_ready = false;

void Build() {
    for (int v = 0; v < 256; ++v) {
        float c = static_cast<float>(v) / 255.0f;
        c = (c - 0.5f) * g_look.contrast + 0.5f;
        c *= g_look.brightness;
        g_table[v] = static_cast<std::uint8_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    g_plain = std::fabs(g_look.brightness - 1.0f) < 0.005f && std::fabs(g_look.contrast - 1.0f) < 0.005f &&
              std::fabs(g_look.saturation - 1.0f) < 0.005f;
    g_ready = true;
}

}  // namespace

Look& Get() {
    return g_look;
}

void Changed() {
    Build();
    ++g_version;
}

std::uint32_t Version() {
    return g_version;
}

bool Plain() {
    if (!g_ready) Build();
    return g_plain;
}

std::uint32_t Grade(std::uint32_t rgba) {
    if (!g_ready) Build();
    if (rgba == 0) return 0;
    int r = static_cast<int>(rgba & 0xFF), g = static_cast<int>((rgba >> 8) & 0xFF), b = static_cast<int>((rgba >> 16) & 0xFF);
    if (std::fabs(g_look.saturation - 1.0f) >= 0.005f) {
        // Saturation: mix each channel with the gray of the same brightness.
        const float lum = 0.299f * static_cast<float>(r) + 0.587f * static_cast<float>(g) + 0.114f * static_cast<float>(b);
        const float s = g_look.saturation;
        r = std::clamp(static_cast<int>(lum + (static_cast<float>(r) - lum) * s), 0, 255);
        g = std::clamp(static_cast<int>(lum + (static_cast<float>(g) - lum) * s), 0, 255);
        b = std::clamp(static_cast<int>(lum + (static_cast<float>(b) - lum) * s), 0, 255);
    }
    return static_cast<std::uint32_t>(g_table[r]) | (static_cast<std::uint32_t>(g_table[g]) << 8) |
           (static_cast<std::uint32_t>(g_table[b]) << 16) | (rgba & 0xFF000000u);
}

std::uint32_t Background() {
    const auto ch = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(g_look.background[0]) | (ch(g_look.background[1]) << 8) | (ch(g_look.background[2]) << 16) | 0xFF000000u;
}

void Save(nlohmann::json& out) {
    out = {{"relief", g_look.relief},
           {"heightTint", g_look.heightTint},
           {"waterDepth", g_look.waterDepth},
           {"shores", g_look.shores},
           {"texture", g_look.texture},
           {"brightness", g_look.brightness},
           {"contrast", g_look.contrast},
           {"saturation", g_look.saturation},
           {"background", {g_look.background[0], g_look.background[1], g_look.background[2]}}};
}

void Load(const nlohmann::json& in) {
    if (!in.is_object()) return;
    g_look.relief = std::clamp(in.value("relief", 1.0f), 0.0f, 2.0f);
    g_look.heightTint = in.value("heightTint", true);
    g_look.waterDepth = in.value("waterDepth", true);
    g_look.shores = in.value("shores", true);
    g_look.texture = in.value("texture", true);
    g_look.brightness = std::clamp(in.value("brightness", 1.0f), 0.5f, 1.5f);
    g_look.contrast = std::clamp(in.value("contrast", 1.0f), 0.5f, 1.5f);
    g_look.saturation = std::clamp(in.value("saturation", 1.0f), 0.0f, 2.0f);
    if (in.contains("background") && in["background"].is_array() && in["background"].size() == 3) {
        for (int k = 0; k < 3; ++k) g_look.background[k] = std::clamp(in["background"][k].get<float>(), 0.0f, 1.0f);
    }
    Changed();
}

}  // namespace luma::modules::world::maplook
