// =============================================================================
//  tr.cpp - Tabla de traducciones por texto (ver tr.h)
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

// Se arma una sola vez (C++ garantiza que una "static" local se inicializa
// una vez aunque la llamen varios hilos a la vez) y después solo se lee.
const std::unordered_map<std::string_view, const Entry*>& Index() {
    static const std::unordered_map<std::string_view, const Entry*> index = [] {
        std::unordered_map<std::string_view, const Entry*> m;
        m.reserve(sizeof(kTable) / sizeof(kTable[0]));
        for (const Entry& e : kTable) m.emplace(e.es, &e);
        return m;
    }();
    return index;
}

}  // namespace

const char* Tr(const char* spanish) {
    if (spanish == nullptr) return "";
    const i18n::Language language = i18n::CurrentLanguage();
    if (language == i18n::Language::Spanish) return spanish;
    const auto& index = Index();
    const auto it = index.find(spanish);
    if (it == index.end()) return spanish;
    const char* text = language == i18n::Language::Portuguese ? it->second->pt : it->second->en;
    return text != nullptr && text[0] != '\0' ? text : spanish;
}

}  // namespace luma
