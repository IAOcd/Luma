// =============================================================================
//  skin_pack.cpp - El paquete de skins "Luma" (ver skin_pack.h)
// =============================================================================
#include "skin_pack.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace luma::skinpack {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// Identificadores fijos del paquete (inventados para Luma; así el juego sabe
// que es el MISMO paquete cada vez que lo reescribimos).
constexpr const char* kPackUuid = "4c756d61-536b-696e-0000-000000000001";
constexpr const char* kModuleUuid = "4c756d61-536b-696e-0000-000000000002";

fs::path Roaming() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) path = raw;
    CoTaskMemFree(raw);
    return path;
}

struct Entry {
    std::string id;
    std::string name;
    fs::path file;
    bool slim = false;
};

json ReadJson(const fs::path& path) {
    std::ifstream file(path);
    return file ? json::parse(file, nullptr, false) : json();
}

// Los textos del juego (.lang) son "clave=valor" por línea: sacamos los saltos.
std::string OneLine(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '=') c = ' ';
    }
    return s;
}

}  // namespace

bool Write() {
    const fs::path roaming = Roaming();
    if (roaming.empty()) return false;
    const fs::path library = roaming / L"Luma" / L"skins";
    const fs::path mojang = roaming / L"Minecraft Bedrock" / L"Users" / L"Shared" / L"games" / L"com.mojang";
    std::error_code ec;
    if (!fs::exists(mojang, ec)) return false;  // el juego (GDK) nunca se abrió en esta PC

    // 1. La biblioteca, con la equipada primero.
    const json lib = ReadJson(library / L"library.json");
    const json active = ReadJson(library / L"active.json");
    const std::string activeId = active.is_object() ? active.value("id", std::string()) : std::string();
    std::vector<Entry> entries;
    if (lib.is_object() && lib.contains("skins") && lib["skins"].is_array()) {
        for (const json& j : lib["skins"]) {
            if (!j.is_object()) continue;
            Entry e;
            e.id = j.value("id", std::string());
            e.name = j.value("name", std::string());
            e.file = library / fs::path(j.value("file", std::string()));
            e.slim = j.value("slim", false);
            if (e.id.size() < 16 || !fs::exists(e.file, ec)) continue;
            if (e.id == activeId) {
                entries.insert(entries.begin(), std::move(e));
            } else {
                entries.push_back(std::move(e));
            }
        }
    }

    // 2. La carpeta del paquete (se rehace entera: así no quedan skins borradas).
    const fs::path pack = mojang / L"skin_packs" / L"Luma";
    fs::remove_all(pack, ec);
    if (entries.empty()) return true;  // sin skins: sin paquete
    fs::create_directories(pack / L"texts", ec);
    if (ec) return false;

    json manifest = {
        {"format_version", 2},
        {"header",
         {{"name", "Luma"},
          {"description", "Your Luma skins"},
          {"uuid", kPackUuid},
          {"version", {1, 0, 0}},
          {"min_engine_version", {1, 21, 0}}}},
        {"modules", json::array({{{"type", "skin_pack"}, {"uuid", kModuleUuid}, {"version", {1, 0, 0}}}})},
    };
    std::ofstream(pack / L"manifest.json", std::ios::trunc) << manifest.dump(2);

    json skins = {{"serialize_name", "Luma"}, {"localization_name", "Luma"}, {"skins", json::array()}};
    std::string lang = "skinpack.Luma=Luma\n";
    for (const Entry& e : entries) {
        const std::string key = "luma_" + e.id.substr(0, 16);
        fs::copy_file(e.file, pack / (key + ".png"), fs::copy_options::overwrite_existing, ec);
        if (ec) continue;
        skins["skins"].push_back({{"localization_name", key},
                                  {"geometry", e.slim ? "geometry.humanoid.customSlim" : "geometry.humanoid.custom"},
                                  {"texture", key + ".png"},
                                  {"type", "free"}});
        lang += "skin.Luma." + key + "=" + OneLine(e.id == activeId ? "★ " + e.name : e.name) + "\n";
    }
    std::ofstream(pack / L"skins.json", std::ios::trunc) << skins.dump(2);
    std::ofstream(pack / L"texts" / L"languages.json", std::ios::trunc) << R"(["en_US"])";
    // El mismo texto para todos los idiomas que el juego busca primero.
    std::ofstream(pack / L"texts" / L"en_US.lang", std::ios::binary | std::ios::trunc) << lang;
    return true;
}

}  // namespace luma::skinpack
