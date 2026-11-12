// =============================================================================
//  text_bidi.cpp - Right to left and joined Arabic letters (see text_bidi.h)
// =============================================================================
#include "core/text_bidi.h"

#include <algorithm>
#include <vector>

namespace luma::bidi {

namespace {

// UTF-8 <-> code points (the numbers of the letters).
std::vector<char32_t> Decode(const std::string& s) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        int len = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<std::size_t>(len);
    }
    return out;
}

std::string Encode(const std::vector<char32_t>& cps) {
    std::string out;
    for (char32_t cp : cps) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool IsRtl(char32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

bool IsLtrStrong(char32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 0x00C0 && c < 0x0590);
}

// Arabic letter shapes: {letter, isolated, final, initial, medial} (0 = that shape doesn't exist:
// letters with only 2 shapes don't join to the NEXT letter).
struct Forms {
    char32_t letter, isolated, final, initial, medial;
};
constexpr Forms kForms[] = {
    {0x0621, 0xFE80, 0, 0, 0},           {0x0622, 0xFE81, 0xFE82, 0, 0},       {0x0623, 0xFE83, 0xFE84, 0, 0},
    {0x0624, 0xFE85, 0xFE86, 0, 0},      {0x0625, 0xFE87, 0xFE88, 0, 0},       {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    {0x0627, 0xFE8D, 0xFE8E, 0, 0},      {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, {0x0629, 0xFE93, 0xFE94, 0, 0},
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, {0x062F, 0xFEA9, 0xFEAA, 0, 0},
    {0x0630, 0xFEAB, 0xFEAC, 0, 0},      {0x0631, 0xFEAD, 0xFEAE, 0, 0},       {0x0632, 0xFEAF, 0xFEB0, 0, 0},
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},
    {0x0648, 0xFEED, 0xFEEE, 0, 0},      {0x0649, 0xFEEF, 0xFEF0, 0, 0},       {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
    // Persian / Urdu letters
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}, {0x0698, 0xFB8A, 0xFB8B, 0, 0},
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91}, {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},
};

const Forms* FormsOf(char32_t c) {
    for (const Forms& f : kForms) {
        if (f.letter == c) return &f;
    }
    return nullptr;
}

bool IsTransparent(char32_t c) {  // the short vowel marks: they don't break the joining
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670;
}

// Joins the Arabic letters (picks each letter's shape) and makes the lam-alef ligatures.
std::vector<char32_t> Shape(const std::vector<char32_t>& in) {
    std::vector<char32_t> out;
    out.reserve(in.size());
    const auto n = in.size();
    auto prevLetter = [&](std::size_t i) -> const Forms* {
        while (i > 0) {
            --i;
            if (IsTransparent(in[i])) continue;
            return FormsOf(in[i]);
        }
        return nullptr;
    };
    auto nextLetter = [&](std::size_t i) -> const Forms* {
        for (++i; i < n; ++i) {
            if (IsTransparent(in[i])) continue;
            return FormsOf(in[i]);
        }
        return nullptr;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const Forms* f = FormsOf(in[i]);
        if (f == nullptr) {
            out.push_back(in[i]);
            continue;
        }
        const Forms* p = prevLetter(i);
        const bool joinsBefore = p != nullptr && p->initial != 0;  // the previous one joins to this one
        // Lam + alef: one ligature.
        if (in[i] == 0x0644 && i + 1 < n) {
            const char32_t a = in[i + 1];
            char32_t lig = 0;
            if (a == 0x0622) lig = 0xFEF5;
            else if (a == 0x0623) lig = 0xFEF7;
            else if (a == 0x0625) lig = 0xFEF9;
            else if (a == 0x0627) lig = 0xFEFB;
            if (lig != 0) {
                out.push_back(joinsBefore ? lig + 1 : lig);
                ++i;
                continue;
            }
        }
        const Forms* nx = nextLetter(i);
        const bool joinsAfter = f->initial != 0 && nx != nullptr;
        char32_t shaped = f->isolated;
        if (joinsBefore && joinsAfter) shaped = f->medial;
        else if (joinsBefore) shaped = f->final != 0 ? f->final : f->isolated;
        else if (joinsAfter) shaped = f->initial;
        out.push_back(shaped != 0 ? shaped : in[i]);
    }
    return out;
}

char32_t Mirror(char32_t c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        default: return c;
    }
}

}  // namespace

bool HasRtl(const std::string& utf8) {
    for (char32_t c : Decode(utf8)) {
        if (IsRtl(c)) return true;
    }
    return false;
}

std::string Visual(const std::string& utf8) {
    std::vector<char32_t> cps = Decode(utf8);
    if (std::none_of(cps.begin(), cps.end(), IsRtl)) return utf8;
    cps = Shape(cps);
    // Runs: left-to-right pieces (Latin words, numbers) keep their order
    // inside; everything else is right to left. Then the whole line is turned
    // around (the first run ends at the right).
    std::vector<std::vector<char32_t>> runs;
    std::vector<bool> ltr;
    for (std::size_t i = 0; i < cps.size();) {
        const bool isLtr = IsLtrStrong(cps[i]);
        std::size_t j = i + 1;
        if (isLtr) {
            // A Latin run: letters, numbers and the spaces/punctuation between them.
            while (j < cps.size() && !IsRtl(cps[j])) ++j;
            while (j > i + 1 && !IsLtrStrong(cps[j - 1])) --j;  // trailing spaces/punctuation go back to RTL
        } else {
            while (j < cps.size() && !IsLtrStrong(cps[j])) ++j;
        }
        runs.emplace_back(cps.begin() + static_cast<std::ptrdiff_t>(i), cps.begin() + static_cast<std::ptrdiff_t>(j));
        ltr.push_back(isLtr);
        i = j;
    }
    std::vector<char32_t> out;
    out.reserve(cps.size());
    for (std::size_t r = runs.size(); r-- > 0;) {
        if (ltr[r]) {
            out.insert(out.end(), runs[r].begin(), runs[r].end());
        } else {
            for (std::size_t k = runs[r].size(); k-- > 0;) out.push_back(Mirror(runs[r][k]));
        }
    }
    return Encode(out);
}

}  // namespace luma::bidi
