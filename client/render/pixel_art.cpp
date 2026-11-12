// =============================================================================
//  pixel_art.cpp - Drawing made of Minecraft pixels (see pixel_art.h)
// =============================================================================
#include "render/pixel_art.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/hud.h"
#include "render/sound.h"

namespace luma::render::pixel {


// The game's font with q screen pixels per font pixel (or the smooth one).
PixelText TextAt(int qi) {
    PixelText t;
    t.q = static_cast<float>(qi);
    t.font = render::hud::PixelFontAt(qi);
    t.pixel = t.font != nullptr;
    if (!t.pixel) t.font = render::hud::SmoothFont();
    t.size = t.pixel ? 11.0f * t.q : 9.0f * t.q;
    t.capTop = t.pixel ? 2.0f * t.q : 0.2f * t.size;
    t.capHeight = t.pixel ? 7.0f * t.q : 0.55f * t.size;
    return t;
}

// A text's visible width (the pixel font leaves one pixel after each letter).
float TextWidth(const PixelText& t, const char* text) {
    const float w = t.font->CalcTextSizeA(t.size, FLT_MAX, 0.0f, text).x;
    return t.pixel ? w - t.q : w;
}

ImU32 Shade(const float* rgb, float k, int alpha) {
    return IM_COL32(static_cast<int>(std::clamp(rgb[0] * k, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(rgb[1] * k, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(rgb[2] * k, 0.0f, 1.0f) * 255.0f), alpha);
}

// The banner: a 7 x 8 body (lighter top row, darker bottom, like the game's
// buttons) and a point of 2 steps whose tip touches the place; a hard shadow
// one pixel down-right; the initial inside, white with the game's text
// shadow (dark on light colors).
void Banner(ImDrawList* draw, ImVec2 tip, const PixelText& t, const float* rgb, int alpha, const char* name) {
    const float q = t.q;
    auto rect = [&](ImVec2 o, float x0, float y0, float x1, float y1, ImU32 color) {
        draw->AddRectFilled(ImVec2(o.x + x0 * q, o.y + y0 * q), ImVec2(o.x + x1 * q, o.y + y1 * q), color);
    };
    auto shape = [&](ImVec2 o, ImU32 body, ImU32 top, ImU32 bottom) {
        rect(o, -3, -10, 4, -2, body);
        rect(o, -3, -10, 4, -9, top);
        rect(o, -3, -3, 4, -2, bottom);
        rect(o, -2, -2, 3, -1, bottom);
        rect(o, -1, -1, 2, 0, bottom);
    };
    const ImU32 shadow = IM_COL32(0, 0, 0, alpha * 110 / 255);
    shape(ImVec2(tip.x + q, tip.y + q), shadow, shadow, shadow);
    shape(tip, Shade(rgb, 1.0f, alpha), Shade(rgb, 1.3f, alpha), Shade(rgb, 0.68f, alpha));
    char initial[8] = {0};
    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (first >= 0x80) {
        const int n = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : 2;  // (a letter of another alphabet)
        for (int i = 0; i < n && name[i] != 0; ++i) initial[i] = name[i];
    } else {
        initial[0] = first != 0 ? static_cast<char>(std::toupper(first)) : '?';
    }
    const float lum = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    const ImVec2 tp(std::floor(tip.x + 0.5f * q - TextWidth(t, initial) * 0.5f), tip.y - 9.0f * q - t.capTop);
    if (lum > 0.7f) {
        draw->AddText(t.font, t.size, tp, IM_COL32(40, 40, 40, alpha), initial);
    } else {
        draw->AddText(t.font, t.size, ImVec2(tp.x + q, tp.y + q), Shade(rgb, 0.25f, alpha), initial);
        draw->AddText(t.font, t.size, tp, IM_COL32(255, 255, 255, alpha), initial);
    }
}

// A line of text like the game's name tags: white on a dark see-through
// band, centered on "cx", its bottom at "bottom", kept between minX and maxX.
void NameTag(ImDrawList* draw, float cx, float bottom, const PixelText& t, const char* text, ImU32 color, float minX, float maxX) {
    const float q = t.q;
    const float w = TextWidth(t, text);
    float x0 = std::floor(cx - w * 0.5f) - q;
    x0 = std::clamp(x0, minX, std::max(minX, maxX - w - 2.0f * q));
    bottom = std::floor(bottom);
    draw->AddRectFilled(ImVec2(x0, bottom - t.capHeight - 2.0f * q), ImVec2(x0 + w + 2.0f * q, bottom), IM_COL32(0, 0, 0, 100));
    draw->AddText(t.font, t.size, ImVec2(x0 + q, bottom - t.capHeight - q - t.capTop), color, text);
}


namespace {
ImU32 MixDot(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}
}  // namespace

void Dot(ImDrawList* draw, ImVec2 center, float q, ImU32 color, int shape, int size) {
    // Cell by cell, so it's made of square pixels: inside the shape = color,
    // one cell around it = dark outline.
    const float r = static_cast<float>(size) * 0.5f;
    auto inside = [&](float x, float y, float grow) {
        const float rr = r + grow;
        if (shape == 1) return std::fabs(x) + std::fabs(y) <= rr + 0.01f;
        if (shape == 2) return std::fabs(x) <= rr - 0.49f && std::fabs(y) <= rr - 0.49f;
        return x * x + y * y <= rr * rr + 0.3f;
    };
    const ImVec2 c0(std::floor(center.x), std::floor(center.y));
    const int n = size / 2 + 2;
    for (int gy = -n; gy <= n; ++gy) {
        for (int gx = -n; gx <= n; ++gx) {
            // (cell centers; even sizes sit between pixels)
            const float x = static_cast<float>(gx) + ((size % 2 == 0) ? 0.5f : 0.0f);
            const float y = static_cast<float>(gy) + ((size % 2 == 0) ? 0.5f : 0.0f);
            ImU32 col = 0;
            if (inside(x, y, 0.0f)) {
                col = y > 0.5f ? MixDot(color, IM_COL32(0, 0, 0, 255), 0.22f) : color;  // the lower half a bit darker
            } else if (inside(x, y, 1.0f)) {
                col = IM_COL32(12, 12, 16, 220);
            }
            if (col == 0) continue;
            const ImVec2 a(c0.x + static_cast<float>(gx) * q, c0.y + static_cast<float>(gy) * q);
            draw->AddRectFilled(a, ImVec2(a.x + q, a.y + q), col);
        }
    }
}

void Text(ImDrawList* draw, const PixelText& t, ImVec2 pos, const char* text, ImU32 color, bool shadow) {
    const ImVec2 p(std::floor(pos.x), std::floor(pos.y - t.capTop));
    if (shadow) {
        // The game's text shadow: the same text, a quarter as bright, one pixel down-right.
        const ImU32 dark = IM_COL32(((color >> IM_COL32_R_SHIFT) & 0xFF) / 4, ((color >> IM_COL32_G_SHIFT) & 0xFF) / 4,
                                    ((color >> IM_COL32_B_SHIFT) & 0xFF) / 4, (color >> IM_COL32_A_SHIFT) & 0xFF);
        draw->AddText(t.font, t.size, ImVec2(p.x + t.q, p.y + t.q), dark, text);
    }
    draw->AddText(t.font, t.size, p, color, text);
}

// --- Minecraft-style windows ------------------------------------------------------

void Panel(ImDrawList* draw, ImVec2 a, ImVec2 b, float q) {
    a = ImVec2(std::floor(a.x), std::floor(a.y));
    b = ImVec2(std::floor(b.x), std::floor(b.y));
    draw->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));                                                   // outline
    draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(b.x - q, b.y - q), IM_COL32(85, 85, 85, 255));  // bottom-right edge
    draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(b.x - 2 * q, b.y - 2 * q), IM_COL32(255, 255, 255, 255));  // top-left edge
    draw->AddRectFilled(ImVec2(a.x + 2 * q, a.y + 2 * q), ImVec2(b.x - 2 * q, b.y - 2 * q), IM_COL32(198, 198, 198, 255));  // stone
}

void DarkPanel(ImDrawList* draw, ImVec2 a, ImVec2 b, float q) {
    a = ImVec2(std::floor(a.x), std::floor(a.y));
    b = ImVec2(std::floor(b.x), std::floor(b.y));
    draw->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150));
    draw->AddRect(a, b, IM_COL32(255, 255, 255, 40), 0.0f, 0, q);
}

bool Button(const char* id, const char* label, ImVec2 pos, ImVec2 size, const PixelText& t, bool selected) {
    const float q = t.q;
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    size = ImVec2(std::floor(size.x), std::floor(size.y));
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    // Black outline; the face gray (lit blue-ish under the mouse, like the
    // game's selected button), a light top edge and a dark bottom edge.
    const ImU32 face = held ? IM_COL32(96, 96, 104, 255) : (hovered || selected) ? IM_COL32(126, 136, 191, 255) : IM_COL32(111, 111, 111, 255);
    const ImU32 top = (hovered || selected) ? IM_COL32(188, 198, 255, 255) : IM_COL32(170, 170, 170, 255);
    const ImU32 bottom = (hovered || selected) ? IM_COL32(72, 80, 128, 255) : IM_COL32(60, 60, 60, 255);
    draw->AddRectFilled(pos, b, IM_COL32(0, 0, 0, 255));
    draw->AddRectFilled(ImVec2(pos.x + q, pos.y + q), ImVec2(b.x - q, b.y - q), bottom);
    draw->AddRectFilled(ImVec2(pos.x + q, pos.y + q), ImVec2(b.x - q, b.y - 2 * q), top);
    draw->AddRectFilled(ImVec2(pos.x + 2 * q, pos.y + 2 * q), ImVec2(b.x - 2 * q, b.y - 3 * q), face);
    const float w = TextWidth(t, label);
    const ImU32 color = (hovered || selected) ? IM_COL32(255, 255, 160, 255) : IM_COL32(224, 224, 224, 255);
    Text(draw, t, ImVec2(pos.x + (size.x - w) * 0.5f, pos.y + (size.y - q - t.capHeight) * 0.5f), label, color);
    if (clicked) sound::Play(sound::Effect::Click);
    return clicked;
}

bool Check(const char* id, const char* label, bool* value, ImVec2 pos, const PixelText& t) {
    const float q = t.q;
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    const float box = 10.0f * q;
    const float w = TextWidth(t, label);
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(box + 4 * q + w, box));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        *value = !*value;
        sound::Play(sound::Effect::Click);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // A sunken slot (like an inventory slot) with a check made of pixels.
    draw->AddRectFilled(pos, ImVec2(pos.x + box, pos.y + box), IM_COL32(55, 55, 55, 255));
    draw->AddRectFilled(ImVec2(pos.x + q, pos.y + q), ImVec2(pos.x + box, pos.y + box), IM_COL32(255, 255, 255, 255));
    draw->AddRectFilled(ImVec2(pos.x + q, pos.y + q), ImVec2(pos.x + box - q, pos.y + box - q),
                        hovered ? IM_COL32(160, 160, 160, 255) : IM_COL32(139, 139, 139, 255));
    if (*value) {
        // The check: pixel steps down then up.
        const ImU32 green = IM_COL32(85, 255, 85, 255);
        const int cells[][2] = {{2, 5}, {3, 6}, {4, 7}, {5, 6}, {6, 5}, {7, 4}, {8, 3}};
        for (const auto& c : cells) {
            const ImVec2 at(pos.x + static_cast<float>(c[0]) * q, pos.y + static_cast<float>(c[1]) * q);
            draw->AddRectFilled(ImVec2(at.x, at.y - q), ImVec2(at.x + q, at.y + q), green);
        }
    }
    Text(draw, t, ImVec2(pos.x + box + 4 * q, pos.y + (box - t.capHeight) * 0.5f), label,
         hovered ? IM_COL32(255, 255, 160, 255) : IM_COL32(64, 64, 64, 255), false);
    return clicked;
}

bool Field(const char* id, char* buffer, int size, ImVec2 pos, float width, const PixelText& t) {
    const float q = t.q;
    pos = ImVec2(std::floor(pos.x), std::floor(pos.y));
    const float h = t.capHeight + 8 * q;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Like the game's text boxes: a gray outline and black inside.
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(160, 160, 160, 255));
    draw->AddRectFilled(ImVec2(pos.x + q, pos.y + q), ImVec2(pos.x + width - q, pos.y + h - q), IM_COL32(0, 0, 0, 255));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 3 * q, pos.y + (h - t.size) * 0.5f + q));
    ImGui::PushFont(t.font);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(224, 224, 224, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(width - 6 * q);
    const bool changed = ImGui::InputText(id, buffer, static_cast<std::size_t>(size));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    return changed;
}

// --- Form -------------------------------------------------------------------------

const float kDyes[16][3] = {
    {0.98f, 1.00f, 1.00f}, {0.97f, 0.50f, 0.11f}, {0.78f, 0.31f, 0.74f}, {0.23f, 0.70f, 0.85f},
    {1.00f, 0.85f, 0.24f}, {0.50f, 0.78f, 0.12f}, {0.95f, 0.55f, 0.67f}, {0.28f, 0.31f, 0.32f},
    {0.62f, 0.62f, 0.59f}, {0.09f, 0.61f, 0.61f}, {0.54f, 0.20f, 0.72f}, {0.24f, 0.27f, 0.67f},
    {0.51f, 0.33f, 0.20f}, {0.37f, 0.49f, 0.09f}, {0.69f, 0.18f, 0.15f}, {0.11f, 0.11f, 0.13f}};

namespace {

constexpr ImU32 kLabel = IM_COL32(236, 236, 236, 255);
constexpr ImU32 kDim = IM_COL32(160, 160, 166, 255);
constexpr ImU32 kTrack = IM_COL32(30, 30, 36, 255);

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

}  // namespace

void Form::Section(const char* title) {
    const float q = this->q();
    y += 4 * q;
    if (Visible(12 * q)) {
        Text(draw, text, ImVec2(x, y + 2 * q), title, accent);
        draw->AddRectFilled(ImVec2(x, y + text.capHeight + 5 * q), ImVec2(x + width, y + text.capHeight + 6 * q), Mix(accent, IM_COL32(0, 0, 0, 0), 0.6f));
    }
    y += text.capHeight + 10 * q;
}

void Form::Hint(const char* message) {
    const float q = this->q();
    // Wrapped by words to the column.
    const char* p = message;
    char line[160];
    while (*p != 0) {
        int n = 0, lastSpace = -1;
        while (p[n] != 0 && n < 159) {
            line[n] = p[n];
            line[n + 1] = 0;
            if (p[n] == ' ') lastSpace = n;
            if (TextWidth(hint, line) > width) break;
            ++n;
        }
        if (p[n] != 0 && lastSpace > 0) n = lastSpace + 1;
        std::memcpy(line, p, static_cast<std::size_t>(n));
        line[n] = 0;
        if (Visible(hint.capHeight + 3 * q)) Text(draw, hint, ImVec2(x, y), line, kDim, false);
        y += hint.capHeight + 3 * q;
        p += n;
        if (n == 0) break;
    }
    y += 2 * q;
}

bool Form::Toggle(const char* label, bool* value) {
    const float q = this->q();
    const float h = 13 * q;
    bool changed = false;
    if (Visible(h)) {
        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::InvisibleButton("##t", ImVec2(width, h))) {
            *value = !*value;
            changed = true;
            sound::Play(sound::Effect::Click);
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) draw->AddRectFilled(ImVec2(x - 3 * q, y), ImVec2(x + width + 3 * q, y + h), IM_COL32(255, 255, 255, 18));
        Text(draw, text, ImVec2(x, y + (h - text.capHeight) * 0.5f), label, kLabel);
        // The switch: a pixel track, the knob on the right (accent) when on.
        const float sw = 16 * q, sh = 8 * q;
        const ImVec2 a(x + width - sw, y + (h - sh) * 0.5f);
        draw->AddRectFilled(a, ImVec2(a.x + sw, a.y + sh), IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + sw - q, a.y + sh - q), *value ? accent : kTrack);
        const float kx = *value ? a.x + sw - 7 * q : a.x + q;
        draw->AddRectFilled(ImVec2(kx, a.y + q), ImVec2(kx + 6 * q, a.y + sh - q), *value ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 120, 126, 255));
        draw->AddRectFilled(ImVec2(kx, a.y + sh - 2 * q), ImVec2(kx + 6 * q, a.y + sh - q), IM_COL32(0, 0, 0, 70));
    }
    y += h + q;
    return changed;
}

bool Form::Slider(const char* label, float* value, float min, float max, const char* format) {
    const float q = this->q();
    const float h = 22 * q;
    bool changed = false;
    if (Visible(h)) {
        char shown[32];
        std::snprintf(shown, sizeof(shown), format, *value);
        Text(draw, text, ImVec2(x, y + 2 * q), label, kLabel);
        const float vw = TextWidth(text, shown);
        Text(draw, text, ImVec2(x + width - vw, y + 2 * q), shown, kDim);
        const ImVec2 a(x, y + 13 * q);
        const float tw = width, th = 6 * q;
        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y - 2 * q));
        ImGui::InvisibleButton("##s", ImVec2(tw, th + 4 * q));
        if (ImGui::IsItemActive()) {
            const float t = std::clamp((ImGui::GetIO().MousePos.x - a.x) / tw, 0.0f, 1.0f);
            const float v = min + (max - min) * t;
            if (v != *value) {
                *value = v;
                changed = true;
            }
        }
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::PopID();
        const float t = std::clamp((*value - min) / (max - min), 0.0f, 1.0f);
        draw->AddRectFilled(a, ImVec2(a.x + tw, a.y + th), IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + tw - q, a.y + th - q), kTrack);
        draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + q + (tw - 2 * q) * t, a.y + th - q), accent);
        const float kx = std::floor(a.x + (tw - 5 * q) * t);
        draw->AddRectFilled(ImVec2(kx, a.y - 2 * q), ImVec2(kx + 5 * q, a.y + th + 2 * q), IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled(ImVec2(kx + q, a.y - q), ImVec2(kx + 4 * q, a.y + th + q), hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(210, 210, 214, 255));
    }
    y += h + q;
    return changed;
}

bool Form::SliderInt(const char* label, int* value, int min, int max, const char* format) {
    float v = static_cast<float>(*value);
    char fmt[32];
    std::snprintf(fmt, sizeof(fmt), "%s", format);
    // (shows the whole number)
    const int before = *value;
    const float q = this->q();
    const float h = 22 * q;
    if (!Visible(h)) {
        y += h + q;
        return false;
    }
    char shown[32];
    std::snprintf(shown, sizeof(shown), fmt, *value);
    Text(draw, text, ImVec2(x, y + 2 * q), label, kLabel);
    Text(draw, text, ImVec2(x + width - TextWidth(text, shown), y + 2 * q), shown, kDim);
    const ImVec2 a(x, y + 13 * q);
    const float tw = width, th = 6 * q;
    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y - 2 * q));
    ImGui::InvisibleButton("##s", ImVec2(tw, th + 4 * q));
    if (ImGui::IsItemActive()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - a.x) / tw, 0.0f, 1.0f);
        v = static_cast<float>(min) + static_cast<float>(max - min) * t;
        *value = std::clamp(static_cast<int>(std::lround(v)), min, max);
    }
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
    ImGui::PopID();
    const float t = max > min ? static_cast<float>(*value - min) / static_cast<float>(max - min) : 0.0f;
    draw->AddRectFilled(a, ImVec2(a.x + tw, a.y + th), IM_COL32(0, 0, 0, 255));
    draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + tw - q, a.y + th - q), kTrack);
    draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + q + (tw - 2 * q) * t, a.y + th - q), accent);
    const float kx = std::floor(a.x + (tw - 5 * q) * t);
    draw->AddRectFilled(ImVec2(kx, a.y - 2 * q), ImVec2(kx + 5 * q, a.y + th + 2 * q), IM_COL32(0, 0, 0, 255));
    draw->AddRectFilled(ImVec2(kx + q, a.y - q), ImVec2(kx + 4 * q, a.y + th + q), hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(210, 210, 214, 255));
    y += h + q;
    return *value != before;
}

bool Form::Choice(const char* label, int* value, const char* const* options, int count) {
    const float q = this->q();
    const float h = 26 * q;
    bool changed = false;
    if (Visible(h)) {
        Text(draw, text, ImVec2(x, y + 2 * q), label, kLabel);
        const float bh = 12 * q, gap = 2 * q;
        const float bw = std::floor((width - gap * static_cast<float>(count - 1)) / static_cast<float>(count));
        ImGui::PushID(label);
        for (int i = 0; i < count; ++i) {
            const ImVec2 a(x + static_cast<float>(i) * (bw + gap), y + 12 * q);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(a);
            if (ImGui::InvisibleButton("##c", ImVec2(bw, bh)) && *value != i) {
                *value = i;
                changed = true;
                sound::Play(sound::Effect::Click);
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = *value == i;
            draw->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), IM_COL32(0, 0, 0, 255));
            draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + bw - q, a.y + bh - q),
                                on ? accent : hovered ? IM_COL32(70, 70, 78, 255) : IM_COL32(44, 44, 52, 255));
            draw->AddRectFilled(ImVec2(a.x + q, a.y + bh - 2 * q), ImVec2(a.x + bw - q, a.y + bh - q), IM_COL32(0, 0, 0, 80));
            const char* opt = options[i];
            const PixelText& t = TextWidth(text, opt) <= bw - 4 * q ? text : hint;
            Text(draw, t, ImVec2(a.x + (bw - TextWidth(t, opt)) * 0.5f, a.y + (bh - q - t.capHeight) * 0.5f), opt,
                 on ? IM_COL32(255, 255, 255, 255) : kLabel);
        }
        ImGui::PopID();
    }
    y += h + q;
    return changed;
}

bool Form::Colors(const char* label, float* rgb) {
    const float q = this->q();
    const float sw = std::floor((width - 7 * 2 * q) / 8.0f);
    const float h = 12 * q + 2 * sw + 2 * q;
    bool changed = false;
    if (Visible(h)) {
        Text(draw, text, ImVec2(x, y + 2 * q), label, kLabel);
        ImGui::PushID(label);
        for (int k = 0; k < 16; ++k) {
            const ImVec2 a(x + static_cast<float>(k % 8) * (sw + 2 * q), y + 12 * q + static_cast<float>(k / 8) * (sw + 2 * q));
            ImGui::PushID(k);
            ImGui::SetCursorScreenPos(a);
            if (ImGui::InvisibleButton("##d", ImVec2(sw, sw))) {
                std::copy(kDyes[k], kDyes[k] + 3, rgb);
                changed = true;
                sound::Play(sound::Effect::Click);
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool same = std::fabs(rgb[0] - kDyes[k][0]) + std::fabs(rgb[1] - kDyes[k][1]) + std::fabs(rgb[2] - kDyes[k][2]) < 0.02f;
            draw->AddRectFilled(a, ImVec2(a.x + sw, a.y + sw), same ? accent : hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
            draw->AddRectFilled(ImVec2(a.x + q, a.y + q), ImVec2(a.x + sw - q, a.y + sw - q), Shade(kDyes[k], 1.0f, 255));
            if (same) draw->AddRectFilled(ImVec2(a.x + sw * 0.5f - q, a.y + sw * 0.5f - q), ImVec2(a.x + sw * 0.5f + q, a.y + sw * 0.5f + q),
                                          IM_COL32(0, 0, 0, 200));
        }
        ImGui::PopID();
    }
    y += h + q;
    return changed;
}

bool Form::ButtonRow(const char* label, bool primary) {
    const float q = this->q();
    const float h = 14 * q;
    bool clicked = false;
    if (Visible(h)) {
        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        clicked = ImGui::InvisibleButton("##b", ImVec2(width, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImGui::PopID();
        const ImU32 face = primary ? (hovered ? Mix(accent, IM_COL32(255, 255, 255, 255), 0.2f) : accent)
                                   : (held ? IM_COL32(36, 36, 42, 255) : hovered ? IM_COL32(72, 72, 80, 255) : IM_COL32(50, 50, 58, 255));
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + h), IM_COL32(0, 0, 0, 255));
        draw->AddRectFilled(ImVec2(x + q, y + q), ImVec2(x + width - q, y + h - q), face);
        draw->AddRectFilled(ImVec2(x + q, y + q), ImVec2(x + width - q, y + 2 * q), IM_COL32(255, 255, 255, hovered ? 60 : 30));
        draw->AddRectFilled(ImVec2(x + q, y + h - 2 * q), ImVec2(x + width - q, y + h - q), IM_COL32(0, 0, 0, 90));
        Text(draw, text, ImVec2(x + (width - TextWidth(text, label)) * 0.5f, y + (h - q - text.capHeight) * 0.5f), label,
             IM_COL32(255, 255, 255, 255));
        if (clicked) sound::Play(sound::Effect::Click);
    }
    y += h + 2 * q;
    return clicked;
}

bool Form::Input(const char* id, char* buffer, int size, const char* placeholder) {
    const float q = this->q();
    const float h = text.capHeight + 8 * q;
    bool changed = false;
    if (Visible(h)) {
        changed = Field(id, buffer, size, ImVec2(x, y), width, text);
        if (buffer[0] == 0 && !ImGui::IsItemActive() && placeholder != nullptr) {
            Text(draw, text, ImVec2(x + 3 * q, y + (h - text.capHeight) * 0.5f), placeholder, IM_COL32(110, 110, 116, 255), false);
        }
    }
    y += h + 3 * q;
    return changed;
}

void Form::Space(float units) {
    y += units * q();
}

}  // namespace luma::render::pixel
