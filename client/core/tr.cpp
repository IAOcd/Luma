// =============================================================================
//  tr.cpp - Translation table looked up by text (see tr.h)
// =============================================================================
#include "core/tr.h"

#include <string_view>
#include <unordered_map>

#include "i18n.h"

namespace luma {

namespace {

struct Entry {
    const char* es;
    const char* en;
    const char* pt;
};

constexpr Entry kTable[] = {
#include "core/tr_table.inc"
};

// Built only once (C++ guarantees a local "static" is initialized once, even
// if several threads call this at the same time) and only read afterwards.
// Every row is indexed by its English text AND by its Spanish text, so the
// code can pass either one. emplace() never overwrites, so if two rows share
// the same English text, the first one wins.
const std::unordered_map<std::string_view, const Entry*>& Index() {
    static const std::unordered_map<std::string_view, const Entry*> index = [] {
        std::unordered_map<std::string_view, const Entry*> m;
        m.reserve(2 * sizeof(kTable) / sizeof(kTable[0]));
        for (const Entry& e : kTable) {
            m.emplace(e.en, &e);
            m.emplace(e.es, &e);
        }
        return m;
    }();
    return index;
}

}  // namespace

const char* Tr(const char* text) {
    if (text == nullptr) return "";
    const auto& index = Index();
    const auto it = index.find(text);
    if (it == index.end()) return text;  // not in the table: show it as written
    const char* translated = nullptr;
    switch (i18n::CurrentLanguage()) {
        case i18n::Language::Spanish: translated = it->second->es; break;
        case i18n::Language::Portuguese: translated = it->second->pt; break;
        default: translated = it->second->en; break;
    }
    return translated != nullptr && translated[0] != '\0' ? translated : text;
}

}  // namespace luma
