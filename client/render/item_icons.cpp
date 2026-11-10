// =============================================================================
//  item_icons.cpp - Implementación de los íconos de ítems (ver item_icons.h)
// =============================================================================
#include "render/item_icons.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/logger.h"
#include "render/gpu.h"

using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace luma::render::icons_items {

namespace {

// Los íconos miden 16x16; los agrandamos 4 veces "a lo bloque" (cada píxel
// se vuelve un cuadrado de 4x4) para que al dibujarlos grandes se vean
// nítidos y no borrosos.
constexpr UINT kUpscale = 4;
constexpr std::size_t kMaxIcons = 96;  // hay lugares limitados para texturas en la GPU

// Dónde está un archivo dentro de un .brarchive.
struct Entry {
    std::filesystem::path file;
    std::uint32_t offset = 0;  // relativa al inicio de los datos
    std::uint32_t size = 0;
};

enum class Status { Requested, Decoded, Ready, Missing };

struct Request {
    std::string id;     // "minecraft:leather_helmet"
    std::string atlas;  // nombre de ícono que guarda el ítem ("helmet"); vacío = bloque o desconocido
    int color = -1;     // color de teñido (cuero), -1 = ninguno
};

struct Icon {
    Request request;
    Status status = Status::Requested;
    std::vector<std::uint8_t> rgba;
    UINT width = 0;
    UINT height = 0;
    ImTextureID texture = 0;
};

std::mutex g_mutex;
std::unordered_map<std::string, Icon> g_icons;  // por clave (id + ícono + color)

// Índice de archivos y tablas del juego (los arma Poll, en el hilo principal).
bool g_indexBuilt = false;
std::unordered_map<std::string, Entry> g_files;                         // "textures/items/apple.png" -> archivo
std::unordered_map<std::string, std::vector<Entry>> g_jsons;           // las tablas .json de TODOS los paquetes (viejo -> nuevo)
std::unordered_map<std::string, std::vector<std::string>> g_itemAtlas;  // item_texture.json
std::unordered_map<std::string, std::vector<std::string>> g_terrain;    // terrain_texture.json
std::unordered_map<std::string, std::string> g_blockTexture;            // blocks.json: bloque -> clave de terreno

std::string Key(const Request& r) {
    return r.id + "|" + r.atlas + "|" + std::to_string(r.color);
}

// "vanilla_1.26.63" -> {1, 26, 63}; "vanilla" -> {} (la base, la más vieja).
std::vector<int> VersionOf(const std::wstring& folder) {
    std::vector<int> v;
    const auto underscore = folder.find(L'_');
    if (underscore == std::wstring::npos) {
        return v;
    }
    int n = -1;
    for (std::size_t i = underscore + 1; i <= folder.size(); ++i) {
        if (i < folder.size() && iswdigit(folder[i])) {
            n = (n < 0 ? 0 : n * 10) + (folder[i] - L'0');
        } else if (n >= 0) {
            v.push_back(n);
            n = -1;
        }
    }
    return v;
}

// Lee un archivo dentro de un .brarchive.
bool ReadEntry(const Entry& e, std::vector<unsigned char>& out) {
    std::ifstream in(e.file, std::ios::binary);
    char header[16];
    std::uint32_t count = 0;
    if (!in.read(header, 16)) {
        return false;
    }
    std::memcpy(&count, header + 8, 4);
    if (e.size == 0 || e.size > (8u << 20)) {
        return false;
    }
    out.resize(e.size);
    in.seekg(16 + static_cast<std::streamoff>(count) * 256 + e.offset);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())));
}

// Agrega los archivos de un .brarchive al índice ("prefix" = su carpeta,
// por ejemplo "textures/items/"). Los paquetes nuevos pisan a los viejos.
void IndexArchive(const std::filesystem::path& file, const std::string& prefix) {
    std::ifstream in(file, std::ios::binary);
    char header[16];
    if (!in.read(header, 16)) {
        return;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, header + 8, 4);
    if (count == 0 || count > 20000) {
        return;
    }
    std::vector<unsigned char> table(static_cast<std::size_t>(count) * 256);
    if (!in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()))) {
        return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const unsigned char* e = table.data() + static_cast<std::size_t>(i) * 256;
        std::string name(reinterpret_cast<const char*>(e + 1), std::min<std::size_t>(e[0], 247));
        Entry entry;
        entry.file = file;
        std::memcpy(&entry.offset, e + 248, 4);
        std::memcpy(&entry.size, e + 252, 4);
        g_files[prefix + name] = entry;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
            g_jsons[prefix + name].push_back(entry);
        }
    }
}

// Lee un JSON del juego (tienen comentarios //, que nlohmann puede ignorar).
bool ParseJson(const std::vector<unsigned char>& bytes, json& out) {
    out = json::parse(bytes.begin(), bytes.end(), nullptr, false, true);
    return !out.is_discarded();
}

// "textures": "x" | ["x", "y"] | {"path": "x"} | [{"path": "x"}] -> lista de rutas
std::vector<std::string> TexturePaths(const json& textures) {
    std::vector<std::string> out;
    auto one = [&](const json& t) {
        if (t.is_string()) {
            out.push_back(t.get<std::string>());
        } else if (t.is_object() && t.contains("path") && t["path"].is_string()) {
            out.push_back(t["path"].get<std::string>());
        }
    };
    if (textures.is_array()) {
        for (const auto& t : textures) {
            one(t);
        }
    } else {
        one(textures);
    }
    return out;
}

void LoadAtlas(const std::string& file, std::unordered_map<std::string, std::vector<std::string>>& into) {
    // Cada paquete trae su tabla (las nuevas agregan o cambian entradas):
    // se leen todas, de la más vieja a la más nueva.
    auto it = g_jsons.find(file);
    if (it == g_jsons.end()) {
        return;
    }
    for (const Entry& entry : it->second) {
        std::vector<unsigned char> bytes;
        json j;
        if (!ReadEntry(entry, bytes) || !ParseJson(bytes, j) || !j.contains("texture_data") ||
            !j["texture_data"].is_object()) {
            continue;
        }
        for (auto& [key, value] : j["texture_data"].items()) {
            if (value.is_object() && value.contains("textures")) {
                into[key] = TexturePaths(value["textures"]);
            }
        }
    }
}

void BuildIndex() {
    g_indexBuilt = true;
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path packs = std::filesystem::path(exe).parent_path() / L"data" / L"resource_packs";
    std::error_code ec;
    std::vector<std::filesystem::path> folders;
    for (const auto& dir : std::filesystem::directory_iterator(packs, ec)) {
        const std::wstring name = dir.path().filename().wstring();
        if (dir.is_directory(ec) && name.rfind(L"vanilla", 0) == 0 && name != L"vanilla_music" && name != L"vanilla_trial") {
            folders.push_back(dir.path());
        }
    }
    std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) {
        return VersionOf(a.filename().wstring()) < VersionOf(b.filename().wstring());
    });
    for (const auto& folder : folders) {
        const auto archives = folder / L"__brarchive";
        IndexArchive(archives / L"textures.brarchive", "textures/");
        IndexArchive(archives / L"textures" / L"items.brarchive", "textures/items/");
        IndexArchive(archives / L"textures" / L"blocks.brarchive", "textures/blocks/");
    }
    LoadAtlas("textures/item_texture.json", g_itemAtlas);
    LoadAtlas("textures/terrain_texture.json", g_terrain);
    // blocks.json (archivo normal en la raíz de cada paquete): qué textura usa
    // cada bloque. Para el ícono usamos la cara "de lado" (o la de arriba).
    for (const auto& folder : folders) {
        std::ifstream in(folder / L"blocks.json", std::ios::binary);
        if (!in) {
            continue;
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        json j;
        if (!ParseJson(bytes, j) || !j.is_object()) {
            continue;
        }
        for (auto& [block, value] : j.items()) {
            if (!value.is_object()) {
                continue;
            }
            const json* t = value.contains("carried_textures") ? &value["carried_textures"]
                            : value.contains("textures")       ? &value["textures"]
                                                               : nullptr;
            if (t == nullptr) {
                continue;
            }
            if (t->is_string()) {
                g_blockTexture[block] = t->get<std::string>();
            } else if (t->is_object()) {
                for (const char* face : {"side", "north", "up"}) {
                    if (t->contains(face) && (*t)[face].is_string()) {
                        g_blockTexture[block] = (*t)[face].get<std::string>();
                        break;
                    }
                }
            }
        }
    }
    log::Info("Íconos de ítems: {} archivos, {} íconos de ítems y {} de bloques en la instalación del juego.",
              g_files.size(), g_itemAtlas.size(), g_terrain.size());
}

// "minecraft:golden_helmet" -> ["gold", "helmet"] (palabras, con los nombres
// que usan las texturas: golden -> gold, wooden -> wood).
std::vector<std::string> Words(const std::string& id) {
    std::string name = id.substr(id.find(':') == std::string::npos ? 0 : id.find(':') + 1);
    std::vector<std::string> words;
    std::stringstream ss(name);
    std::string w;
    while (std::getline(ss, w, '_')) {
        if (w == "golden") w = "gold";
        if (w == "wooden") w = "wood";
        if (!w.empty()) words.push_back(w);
    }
    return words;
}

// De varias texturas posibles (por ejemplo, todos los cascos), la que más
// palabras comparte con el id del ítem.
std::string BestVariant(const std::vector<std::string>& paths, const std::string& id) {
    if (paths.empty()) {
        return {};
    }
    const std::vector<std::string> words = Words(id);
    std::string best = paths[0];
    int bestScore = -1;
    for (const std::string& p : paths) {
        const std::string file = p.substr(p.find_last_of('/') + 1);
        int score = 0;
        for (const std::string& w : words) {
            if (file.find(w) != std::string::npos) {
                score += static_cast<int>(w.size());
            }
        }
        if (score > bestScore) {
            bestScore = score;
            best = p;
        }
    }
    return best;
}

// La ruta de la textura ("textures/items/leather_helmet") para un pedido.
// Para cuando no sabemos el nombre de ícono que guarda el ítem (por ejemplo,
// los del selector del contador, que no salen de tu inventario): los que no
// se llaman igual que su id.
const char* KnownAtlas(const std::string& name) {
    static const std::pair<const char*, const char*> kAliases[] = {
        {"golden_apple", "apple_golden"},      {"enchanted_golden_apple", "apple_golden"},
        {"totem_of_undying", "totem"},         {"firework_rocket", "fireworks"},
        {"cooked_beef", "beef_cooked"},        {"cooked_porkchop", "porkchop_cooked"},
        {"cooked_chicken", "chicken_cooked"},  {"golden_carrot", "carrot_golden"},
        {"redstone", "redstone_dust"},         {"lapis_lazuli", "dye_powder"},
        {"slime_ball", "slimeball"},           {"experience_bottle", "experience_bottle"},
        {"bow", "bow_standby"},                {"water_bucket", "bucket"},
        {"lava_bucket", "bucket"},
    };
    for (const auto& [id, atlas] : kAliases) {
        if (name == id) {
            return atlas;
        }
    }
    return nullptr;
}

std::string ResolvePath(const Request& r) {
    const std::string name = r.id.substr(r.id.find(':') == std::string::npos ? 0 : r.id.find(':') + 1);
    const char* known = KnownAtlas(name);
    // 1. Ítem: su nombre de ícono en item_texture.json.
    for (const std::string& atlas : {r.atlas, std::string(known != nullptr ? known : ""), name}) {
        auto it = g_itemAtlas.find(atlas);
        if (!atlas.empty() && it != g_itemAtlas.end()) {
            return BestVariant(it->second, r.id);
        }
    }
    // 2. Bloque: blocks.json -> terrain_texture.json.
    auto block = g_blockTexture.find(name);
    const std::string terrainKey = block != g_blockTexture.end() ? block->second : name;
    auto terrain = g_terrain.find(terrainKey);
    if (terrain != g_terrain.end() && !terrain->second.empty()) {
        return terrain->second[0];
    }
    // 3. Último intento: un archivo con el mismo nombre.
    for (const char* dir : {"textures/items/", "textures/blocks/"}) {
        if (g_files.count(std::string(dir) + name + ".png") || g_files.count(std::string(dir) + name + ".tga")) {
            return std::string(dir) + name;
        }
    }
    return {};
}

bool DecodeWic(const std::vector<unsigned char>& png, std::vector<std::uint8_t>& out, UINT& w, UINT& h) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> converted;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted)) ||
        FAILED(converted->GetSize(&w, &h)) || w == 0 || h == 0 || w > 512 || h > 512) {
        return false;
    }
    out.resize(static_cast<std::size_t>(w) * h * 4);
    return SUCCEEDED(converted->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()));
}

// TGA (el formato de las texturas teñibles, como la armadura de cuero): sin
// comprimir o comprimido "RLE", de 24 o 32 bits por píxel.
bool DecodeTga(const std::vector<unsigned char>& t, std::vector<std::uint8_t>& out, UINT& w, UINT& h) {
    if (t.size() < 18) {
        return false;
    }
    const int idLength = t[0];
    const int type = t[2];
    w = static_cast<UINT>(t[12] | (t[13] << 8));
    h = static_cast<UINT>(t[14] | (t[15] << 8));
    const int bpp = t[16];
    const bool topToBottom = (t[17] & 0x20) != 0;
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || w == 0 || h == 0 || w > 512 || h > 512) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(bpp / 8);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
    std::size_t src = 18 + static_cast<std::size_t>(idLength);
    std::size_t done = 0;
    const std::size_t total = static_cast<std::size_t>(w) * h;
    auto put = [&](std::size_t at) {
        // El archivo guarda BGR(A); nosotros queremos RGBA.
        pixels[done * 4 + 0] = t[at + 2];
        pixels[done * 4 + 1] = t[at + 1];
        pixels[done * 4 + 2] = t[at + 0];
        pixels[done * 4 + 3] = bytes == 4 ? t[at + 3] : 255;
        ++done;
    };
    while (done < total) {
        if (type == 2) {
            if (src + bytes > t.size()) return false;
            put(src);
            src += bytes;
        } else {
            if (src >= t.size()) return false;
            const int header = t[src++];
            const int count = (header & 0x7F) + 1;
            if (header & 0x80) {  // un píxel repetido
                if (src + bytes > t.size()) return false;
                for (int i = 0; i < count && done < total; ++i) put(src);
                src += bytes;
            } else {  // varios píxeles distintos
                for (int i = 0; i < count && done < total; ++i) {
                    if (src + bytes > t.size()) return false;
                    put(src);
                    src += bytes;
                }
            }
        }
    }
    // Normalmente la primera fila del archivo es la de ABAJO: la damos vuelta.
    if (!topToBottom) {
        const std::size_t row = static_cast<std::size_t>(w) * 4;
        for (UINT y = 0; y < h / 2; ++y) {
            std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(y * row),
                             pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                             pixels.begin() + static_cast<std::ptrdiff_t>((h - 1 - y) * row));
        }
    }
    out = std::move(pixels);
    return true;
}

// Teñido como lo hace el juego con la armadura de cuero: los píxeles con
// alfa 255 se multiplican por el color; los de alfa muy bajo (pero no 0)
// son partes que NO se tiñen (se ven con su color); alfa 0 = transparente.
void Tint(std::vector<std::uint8_t>& px, int color, bool isTga) {
    const int r = (color >> 16) & 0xFF;
    const int g = (color >> 8) & 0xFF;
    const int b = color & 0xFF;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const int a = px[i + 3];
        if (a == 0) {
            continue;
        }
        if (!isTga || a == 255) {
            px[i + 0] = static_cast<std::uint8_t>(px[i + 0] * r / 255);
            px[i + 1] = static_cast<std::uint8_t>(px[i + 1] * g / 255);
            px[i + 2] = static_cast<std::uint8_t>(px[i + 2] * b / 255);
        }
        px[i + 3] = 255;
    }
}

bool Load(const Request& r, Icon& out) {
    const std::string path = ResolvePath(r);
    if (path.empty()) {
        return false;
    }
    std::vector<unsigned char> bytes;
    std::vector<std::uint8_t> pixels;
    UINT w = 0;
    UINT h = 0;
    bool isTga = false;
    auto png = g_files.find(path + ".png");
    auto tga = g_files.find(path + ".tga");
    // Para el cuero teñido preferimos el TGA (tiene la máscara de teñido).
    if (r.color >= 0 && tga != g_files.end() && ReadEntry(tga->second, bytes) && DecodeTga(bytes, pixels, w, h)) {
        isTga = true;
    } else if (png != g_files.end() && ReadEntry(png->second, bytes) && DecodeWic(bytes, pixels, w, h)) {
    } else if (tga != g_files.end() && ReadEntry(tga->second, bytes) && DecodeTga(bytes, pixels, w, h)) {
        isTga = true;
    } else {
        return false;
    }
    // Texturas animadas (varios cuadros uno debajo del otro): el primero.
    if (h > w) {
        h = w;
        pixels.resize(static_cast<std::size_t>(w) * h * 4);
    }
    if (r.color >= 0 || r.id.find("leather_") != std::string::npos) {
        // Sin color guardado, el cuero tiene su marrón de fábrica.
        Tint(pixels, r.color >= 0 ? r.color : 0xA06540, isTga);
    }
    // Agrandar "a lo bloque" (vecino más cercano).
    out.width = w * kUpscale;
    out.height = h * kUpscale;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    for (UINT y = 0; y < out.height; ++y) {
        for (UINT x = 0; x < out.width; ++x) {
            std::memcpy(&out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4],
                        &pixels[(static_cast<std::size_t>(y / kUpscale) * w + x / kUpscale) * 4], 4);
        }
    }
    return true;
}

}  // namespace

ImTextureID Get(const char* id, const char* atlas, int color) {
    Request r{id, atlas != nullptr ? atlas : "", color};
    const std::string key = Key(r);
    std::lock_guard lock(g_mutex);
    auto it = g_icons.find(key);
    if (it == g_icons.end()) {
        if (g_icons.size() < kMaxIcons) {
            Icon icon;
            icon.request = std::move(r);
            g_icons.emplace(key, std::move(icon));  // pedido: lo resuelve Poll
        }
        return 0;
    }
    return it->second.status == Status::Ready ? it->second.texture : 0;
}

void Poll() {
    // Copiar los pedidos para no tener el candado mientras se lee el disco.
    std::vector<std::pair<std::string, Request>> pending;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [key, icon] : g_icons) {
            if (icon.status == Status::Requested) {
                pending.emplace_back(key, icon.request);
            }
        }
    }
    if (pending.empty()) {
        return;
    }
    if (!g_indexBuilt) {
        BuildIndex();
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (const auto& [key, request] : pending) {
        Icon decoded;
        const bool ok = Load(request, decoded);
        std::lock_guard lock(g_mutex);
        Icon& icon = g_icons[key];
        if (ok) {
            icon.rgba = std::move(decoded.rgba);
            icon.width = decoded.width;
            icon.height = decoded.height;
            icon.status = Status::Decoded;
        } else {
            icon.status = Status::Missing;
        }
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

void UploadPending() {
    std::lock_guard lock(g_mutex);
    for (auto& [key, icon] : g_icons) {
        if (icon.status != Status::Decoded) {
            continue;
        }
        icon.texture = gpu::CreateTexture(icon.rgba.data(), icon.width, icon.height);
        icon.status = icon.texture != 0 ? Status::Ready : Status::Missing;
        icon.rgba.clear();
        icon.rgba.shrink_to_fit();
    }
}

}  // namespace luma::render::icons_items
