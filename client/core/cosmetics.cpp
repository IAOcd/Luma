// =============================================================================
//  cosmetics.cpp - Los cosméticos de Luma (ver cosmetics.h)
// =============================================================================
#include "core/cosmetics.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

#include "core/cape_model.h"
#include "core/logger.h"
#include "core/paths.h"
#include "sdk/cape.h"
#include "sdk/player.h"

// La dirección donde Windows cargó Luma.dll (para buscar sus recursos).
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace luma::cosmetics {

namespace {

// --- Pixel art de las capas ----------------------------------------------------------
//  Una capa de Minecraft es una "caja" de 10 x 16 x 1 píxeles, desplegada en
//  una imagen de 64x32 (como una caja de cartón abierta):
//      (1,0)  arriba 10x1        (11,0) abajo 10x1
//      (0,1)  costado 1x16       (1,1)  AFUERA 10x16 (lo que ven los demás)
//      (11,1) costado 1x16       (12,1) adentro 10x16 (contra tu espalda)
//  El resto de la imagen queda transparente.

struct Rgb {
    float r, g, b;
};

Rgb Mix(Rgb a, Rgb b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb Hex(std::uint32_t c) {
    return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
}

void Put(std::vector<std::uint8_t>& img, int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(sdk::cape::kWidth) || y >= static_cast<int>(sdk::cape::kHeight)) return;
    std::uint8_t* p = &img[(static_cast<std::size_t>(y) * sdk::cape::kWidth + x) * 4];
    p[0] = static_cast<std::uint8_t>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    p[1] = static_cast<std::uint8_t>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    p[2] = static_cast<std::uint8_t>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    p[3] = 255;
}

// El color de fondo de un diseño a una altura (0 = arriba, 1 = abajo).
Rgb Background(int design, float t) {
    switch (design) {
        case 1:  // Amanecer: rosa arriba, naranja, dorado abajo
            return t < 0.5f ? Mix(Hex(0xFF8FA3), Hex(0xFF9F43), t * 2.0f)
                            : Mix(Hex(0xFF9F43), Hex(0xFFD166), (t - 0.5f) * 2.0f);
        case 2:  // Noche: azul profundo a violeta
            return Mix(Hex(0x141A38), Hex(0x3B2A63), t);
        default:  // Clásica: ámbar luminoso a naranja (el acento de Luma)
            return Mix(Hex(0xFFC15E), Hex(0xE8661A), t);
    }
}

// La "L" del logo de Luma, 4 x 7 píxeles (1 = pintado).
constexpr const char* kLogo[7] = {"1100", "1100", "1100", "1100", "1100", "1111", "0111"};

// --- Estado ----------------------------------------------------------------------------
std::mutex g_mutex;
std::string g_selected;        // la capa elegida ("" = ninguna)
std::string g_sent;            // lo último que le dijimos al SDK
bool g_sentValid = false;
std::uintptr_t g_sentPlayer = 0;  // para quién se mandó (otro mundo = otra vez)

std::filesystem::path File() {
    return paths::DataDir() / L"cosmetics.json";
}

void Save(const std::string& cape) {
    nlohmann::json j;
    j["cape"] = cape;
    std::ofstream(File(), std::ios::trunc) << j.dump(2);
}

}  // namespace

std::vector<std::uint8_t> DrawnCapeImage(const std::string& id);

namespace {
// Lee un PNG de dentro de Luma.dll (como el logo, render/assets.cpp) con WIC,
// el decodificador de imágenes que trae Windows.
bool DecodeResourcePng(int resource, CapeTexture& out) {
    HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // 10 = RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = found ? SizeofResource(module, found) : 0;
    if (bytes == nullptr || size == 0) return false;
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapSource* converted = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), size)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame, &converted)) &&
        SUCCEEDED(converted->GetSize(&w, &h)) && sdk::cape::ValidSize(w, h)) {
        out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
        ok = SUCCEEDED(converted->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data()));
        out.width = w;
        out.height = h;
    }
    if (converted) converted->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(comInit)) CoUninitialize();
    if (!ok) out = {};
    return ok;
}

std::mutex g_imageMutex;
CapeTexture g_imageCache[std::size(kCapes)];
}  // namespace

CapeTexture CapeImage(const std::string& id) {
    for (std::size_t i = 0; i < std::size(kCapes); ++i) {
        if (id != kCapes[i].id || kCapes[i].resource == 0) continue;
        std::lock_guard lock(g_imageMutex);
        if (g_imageCache[i].rgba.empty() && !DecodeResourcePng(kCapes[i].resource, g_imageCache[i])) {
            log::Warn("Cosméticos: no se pudo leer la capa {} de dentro de Luma.dll.", id);
        }
        return g_imageCache[i];
    }
    CapeTexture out;
    out.rgba = DrawnCapeImage(id);
    if (!out.rgba.empty()) {
        out.width = sdk::cape::kWidth;
        out.height = sdk::cape::kHeight;
    }
    return out;
}

std::vector<std::uint8_t> DrawnCapeImage(const std::string& id) {
    int design = -1;
    for (int i = 0; i < 3; ++i) {  // (las tres primeras son las dibujadas en código)
        if (id == kCapes[i].id) design = i;
    }
    if (design < 0) return {};
    std::vector<std::uint8_t> img(static_cast<std::size_t>(sdk::cape::kWidth) * sdk::cape::kHeight * 4, 0);
    // Las caras grandes (afuera y adentro) y los costados: el degradé.
    for (int y = 0; y < 16; ++y) {
        const Rgb c = Background(design, y / 15.0f);
        const Rgb dark = Mix(c, Hex(0x000000), 0.25f);  // el adentro y los costados, más oscuros
        for (int x = 0; x < 10; ++x) {
            // Borde de afuera un tono más oscuro, como una costura.
            const bool edge = x == 0 || x == 9 || y == 15;
            Put(img, 1 + x, 1 + y, edge ? Mix(c, Hex(0x000000), 0.18f) : c);
            Put(img, 12 + x, 1 + y, dark);
        }
        Put(img, 0, 1 + y, dark);
        Put(img, 11, 1 + y, dark);
    }
    for (int x = 0; x < 10; ++x) {
        Put(img, 1 + x, 0, Background(design, 0.0f));
        Put(img, 11 + x, 0, Background(design, 1.0f));
    }
    if (design == 2) {
        // Noche: estrellas y una luna cálida.
        const int stars[][2] = {{2, 2}, {7, 3}, {4, 6}, {8, 9}, {2, 12}, {6, 13}};
        for (const auto& s : stars) Put(img, 1 + s[0], 1 + s[1], Hex(0xFFF3C4));
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                if (!(x == 2 && y == 1)) Put(img, 1 + 5 + x, 1 + 5 + y, Hex(0xFFD27A));
            }
        }
        return img;
    }
    if (design == 1) {
        // Amanecer: un sol que asoma abajo.
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 6; ++x) {
                const float dx = x - 2.5f, dy = y - 3.5f;
                if (dx * dx + dy * dy <= 9.5f) Put(img, 1 + 2 + x, 1 + 11 + y, Hex(0xFFF1B8));
            }
        }
    }
    // El logo de Luma, centrado arriba.
    const Rgb ink = design == 1 ? Hex(0xFFFFFF) : Hex(0xFFF4DC);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 4; ++x) {
            if (kLogo[y][x] == '1') Put(img, 1 + 3 + x, 1 + 3 + y, ink);
        }
    }
    return img;
}

bool IsLumaCape(const std::string& id) {
    for (const CapeDesign& c : kCapes) {
        if (id == c.id) return true;
    }
    return false;
}

void Load() {
    std::ifstream file(File());
    const nlohmann::json j = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
    std::lock_guard lock(g_mutex);
    g_selected.clear();
    if (j.is_object()) {
        const std::string cape = j.value("cape", std::string());
        if (IsLumaCape(cape)) g_selected = cape;
    }
    // La capa 3D: las imágenes al disco y empezar a dibujar.
    model::PrepareFiles();
    model::SetCape(g_selected);
    model::Start();
}

std::string SelectedCape() {
    std::lock_guard lock(g_mutex);
    return g_selected;
}

void SelectCape(const std::string& id) {
    {
        std::lock_guard lock(g_mutex);
        g_selected = IsLumaCape(id) ? id : std::string();
    }
    model::SetCape(IsLumaCape(id) ? id : std::string());
    Save(IsLumaCape(id) ? id : std::string());
}

void Poll() {
    if (!sdk::cape::Available()) return;
    std::lock_guard lock(g_mutex);
    // Avisarle al SDK cuando cambia la elección, y otra vez al entrar a un
    // mundo (otro jugador): ahí el juego arma tu skin de nuevo, sin la capa.
    const std::uintptr_t player = sdk::player::LocalPlayer();
    const bool newPlayer = player != 0 && player != g_sentPlayer;
    if (g_sentValid && g_sent == g_selected && !newPlayer) return;
    if (player != 0) g_sentPlayer = player;
    g_sent = g_selected;
    g_sentValid = true;
    // La capa de Luma la dibuja Luma (core/cape_model). En tus mundos locales
    // le sacamos a tu skin la capa del juego, para que no asome debajo
    // ("luma_none" = sin capa); con "ninguna", vuelve la que tenías.
    sdk::cape::SetLocalCape({}, 0, 0, g_selected.empty() ? std::string() : std::string("luma_none"));
}

}  // namespace luma::cosmetics
