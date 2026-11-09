// =============================================================================
//  item_icons.cpp - Implementación de los íconos de ítems (ver item_icons.h)
// =============================================================================
#include "render/item_icons.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/logger.h"
#include "core/paths.h"
#include "render/gpu.h"

using Microsoft::WRL::ComPtr;

namespace luma::render::icons_items {

namespace {

// Escala de los píxeles: los íconos miden 16x16; los agrandamos 4 veces
// "a lo bloque" (cada píxel se vuelve un cuadrado de 4x4) para que al
// dibujarlos grandes se vean nítidos y no borrosos.
constexpr UINT kUpscale = 4;
constexpr std::size_t kMaxIcons = 96;  // hay lugares limitados para texturas en la GPU

// Ids cuyo nombre de textura no coincide con el del ítem.
struct Alias {
    const char* id;
    const char* texture;
};
constexpr Alias kAliases[] = {
    {"golden_apple", "apple_golden"},
    {"enchanted_golden_apple", "apple_golden"},
    {"totem_of_undying", "totem"},
    {"firework_rocket", "fireworks"},
    {"cooked_beef", "beef_cooked"},
    {"cooked_porkchop", "porkchop_cooked"},
    {"cooked_chicken", "chicken_cooked"},
    {"golden_carrot", "carrot_golden"},
    {"bow", "bow_standby"},
    {"snowball", "snowball"},
    {"water_bucket", "bucket_water"},
    {"lava_bucket", "bucket_lava"},
};

struct Entry {
    std::filesystem::path file;
    std::uint32_t offset = 0;  // relativa al inicio de los datos
    std::uint32_t size = 0;
};

enum class Status { Requested, Decoded, Ready, Missing };

struct Icon {
    Status status = Status::Requested;
    std::vector<std::uint8_t> rgba;
    UINT width = 0;
    UINT height = 0;
    ImTextureID texture = 0;
};

std::mutex g_mutex;
std::unordered_map<std::string, Icon> g_icons;  // por id
std::unordered_map<std::string, Entry> g_index; // nombre de textura -> dónde está
bool g_indexBuilt = false;

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

// Lee la tabla de un .brarchive y agrega sus PNG al índice.
void IndexArchive(const std::filesystem::path& file) {
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
        const std::size_t len = std::min<std::size_t>(e[0], 247);
        std::string name(reinterpret_cast<const char*>(e + 1), len);
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0) {
            continue;
        }
        name.resize(name.size() - 4);
        Entry entry;
        entry.file = file;
        std::memcpy(&entry.offset, e + 248, 4);
        std::memcpy(&entry.size, e + 252, 4);
        // Los paquetes se recorren del más viejo al más nuevo: el nuevo pisa.
        g_index[name] = entry;
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
    // Primero los bloques y después los ítems: si un nombre está en los dos,
    // gana el dibujo del ítem.
    for (const wchar_t* archive : {L"blocks.brarchive", L"items.brarchive"}) {
        for (const auto& folder : folders) {
            const auto file = folder / L"__brarchive" / L"textures" / archive;
            if (std::filesystem::exists(file, ec)) {
                IndexArchive(file);
            }
        }
    }
    log::Info("Íconos de ítems: {} texturas encontradas en la instalación del juego.", g_index.size());
}

const Entry* Find(const std::string& id) {
    std::string name = id.substr(id.find(':') == std::string::npos ? 0 : id.find(':') + 1);
    for (const Alias& a : kAliases) {
        if (name == a.id) {
            name = a.texture;
            break;
        }
    }
    auto it = g_index.find(name);
    return it != g_index.end() ? &it->second : nullptr;
}

bool Decode(const std::vector<unsigned char>& png, Icon& out) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> converted;
    UINT w = 0;
    UINT h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted)) ||
        FAILED(converted->GetSize(&w, &h)) || w == 0 || h == 0 || w > 256 || h > 256) {
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
    if (FAILED(converted->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
        return false;
    }
    // Algunas texturas son tiras animadas (varios cuadros uno debajo del
    // otro): nos quedamos con el primer cuadro (cuadrado).
    if (h > w) {
        h = w;
        pixels.resize(static_cast<std::size_t>(w) * h * 4);
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

ImTextureID Get(const char* id) {
    std::lock_guard lock(g_mutex);
    auto it = g_icons.find(id);
    if (it == g_icons.end()) {
        if (g_icons.size() < kMaxIcons) {
            g_icons.emplace(id, Icon{});  // pedido: lo resuelve Poll
        }
        return 0;
    }
    return it->second.status == Status::Ready ? it->second.texture : 0;
}

void Poll() {
    // Copiar la lista de pedidos para no tener el candado mientras se lee el disco.
    std::vector<std::string> pending;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& [id, icon] : g_icons) {
            if (icon.status == Status::Requested) {
                pending.push_back(id);
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
    for (const std::string& id : pending) {
        Icon decoded;
        bool ok = false;
        if (const Entry* entry = Find(id)) {
            std::ifstream in(entry->file, std::ios::binary);
            char header[16];
            std::uint32_t count = 0;
            if (in.read(header, 16)) {
                std::memcpy(&count, header + 8, 4);
                std::vector<unsigned char> png(entry->size);
                in.seekg(16 + static_cast<std::streamoff>(count) * 256 + entry->offset);
                ok = entry->size > 0 && entry->size < (1u << 20) &&
                     in.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size())) &&
                     Decode(png, decoded);
            }
        }
        std::lock_guard lock(g_mutex);
        Icon& icon = g_icons[id];
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
    for (auto& [id, icon] : g_icons) {
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
