// =============================================================================
//  font_fallback.cpp - Letters of every language (see font_fallback.h)
// =============================================================================
#include "render/font_fallback.h"

#include <windows.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <imgui_internal.h>  // ImTextCharFromUtf8

#include "core/logger.h"
#include "core/paths.h"
#include "render/theme.h"

namespace luma::render::fontfallback {

namespace {

constexpr std::size_t kMaxLetters = 6000;  // a safety limit for the atlas size

// The groups: which Windows fonts (in order: the first that has a letter wins).
enum Group { kJapanese, kKorean, kChinese, kThai, kIndic, kOther, kGroupCount };
const wchar_t* const kFiles[kGroupCount][4] = {
    {L"YuGothM.ttc", L"msyh.ttc", L"msjh.ttc", nullptr},
    {L"malgun.ttf", nullptr, nullptr, nullptr},
    {L"msyh.ttc", L"msjh.ttc", L"YuGothM.ttc", nullptr},
    {L"LeelawUI.ttf", L"tahoma.ttf", nullptr, nullptr},
    {L"Nirmala.ttc", nullptr, nullptr, nullptr},
    {L"segoeui.ttf", L"tahoma.ttf", L"seguisym.ttf", nullptr},
};

std::set<ImWchar> g_needed;                         // every extra letter asked for (render thread)
std::vector<ImWchar> g_ranges[kGroupCount];         // the lists for the current build (they must live while it's built)
bool g_haveFile[kGroupCount][4]{};
std::filesystem::path g_fontsDir;

Group GroupOf(unsigned int c) {
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF)) return kJapanese;  // kana
    if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) return kKorean;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x3000 && c <= 0x303F) ||
        (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0xF900 && c <= 0xFAFF)) {
        return kChinese;  // ideographs, CJK punctuation, full-width forms
    }
    if (c >= 0x0E00 && c <= 0x0E7F) return kThai;
    if (c >= 0x0900 && c <= 0x0DFF) return kIndic;  // Devanagari, Bengali, Tamil, ...
    return kOther;
}

}  // namespace

void Need(const char* utf8) {
    if (utf8 == nullptr) return;
    bool added = false;
    const char* s = utf8;
    const char* end = s + std::strlen(s);
    while (s < end) {
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, s, end);
        if (n <= 0) break;
        s += n;
        if (c < 0x100 || c > 0xFFFF) continue;  // Latin-1 is always there (and ImGui here is 16-bit)
        if (g_needed.size() >= kMaxLetters) break;
        if (g_needed.insert(static_cast<ImWchar>(c)).second) added = true;
    }
    if (added) theme::RequestFontRebuild();
}

void BeginBuild() {
    wchar_t windowsDir[MAX_PATH];
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    g_fontsDir = std::filesystem::path(windowsDir) / L"Fonts";
    for (int g = 0; g < kGroupCount; ++g) {
        g_ranges[g].clear();
        for (int f = 0; f < 4; ++f) {
            std::error_code ec;
            g_haveFile[g][f] = kFiles[g][f] != nullptr && std::filesystem::exists(g_fontsDir / kFiles[g][f], ec);
        }
    }
    // Each needed letter, in its group's list (as ranges "from, to" of one letter).
    for (ImWchar c : g_needed) {
        auto& r = g_ranges[GroupOf(c)];
        if (!r.empty() && r.back() + 1 == c) {
            r.back() = c;  // extends the last range
        } else {
            r.push_back(c);
            r.push_back(c);
        }
    }
    for (auto& r : g_ranges) {
        if (!r.empty()) r.push_back(0);  // the end of the list
    }
    if (!g_needed.empty()) log::Info("Fonts: {} extra letters (other alphabets) added.", g_needed.size());
}

void MergeInto(ImGuiIO& io, float size) {
    for (int g = 0; g < kGroupCount; ++g) {
        if (g_ranges[g].empty()) continue;
        for (int f = 0; f < 4; ++f) {
            if (!g_haveFile[g][f]) continue;
            ImFontConfig config;
            config.MergeMode = true;   // into the font just added
            config.OversampleH = 1;    // smaller atlas (these are big letters already)
            config.OversampleV = 1;
            config.GlyphRanges = g_ranges[g].data();
            const std::string path = paths::ToUtf8(g_fontsDir / kFiles[g][f]);
            io.Fonts->AddFontFromFileTTF(path.c_str(), size, &config);
        }
    }
}

}  // namespace luma::render::fontfallback
