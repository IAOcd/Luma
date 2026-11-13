// =============================================================================
//  skin_apply.cpp - Cambiar la skin al instante (ver skin_apply.h)
// =============================================================================
#include "sdk/skin_apply.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/paths.h"
#include "core/tr.h"
#include "render/notifications.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"
#include "sdk/players.h"
#include "sdk/scanner.h"

namespace luma::sdk::skinapply {

namespace {

namespace fs = std::filesystem;

// MinecraftPackets::createPacket(shared_ptr<Packet>* out, int id). Firma única en
// 1.26.63 (empieza comparando el id con el último que existe: cmp edx,169h).
constexpr const char* kCreatePacket = "56 48 83 EC ?? 48 89 CE 81 FA";
constexpr int kPlayerSkinPacketId = 93;  // 0x5D

// El paquete de skin (0x98 bytes), visto en su constructor (juego+0x404AF70):
constexpr std::uintptr_t kPacketUuid = 0x30;     // mce::UUID (16 bytes)
constexpr std::uintptr_t kPacketSkin = 0x40;     // shared_ptr<SerializedSkin> (puntero en +0x40)
constexpr std::uintptr_t kPacketNewName = 0x50;  // std::string
constexpr std::uintptr_t kPacketOldName = 0x70;  // std::string
// PacketSender (Player+0x820): [0] destructor, [1] ¿listo?, [2] mandar,
// [3] mandar a alguien, [4] MANDAR AL SERVIDOR.
constexpr std::uintptr_t kPlayerPacketSender = 0x820;
constexpr std::size_t kSendToServerIndex = 4;

constexpr std::uintptr_t kPlayerSkin = 0xAD8;  // Player -> shared_ptr<SerializedSkin>*
constexpr std::size_t kSkinSize = 0x210;       // SerializedSkin (lo reserva el constructor del paquete)
constexpr std::uintptr_t kSkinId = 0x00;       // std::string
constexpr std::uintptr_t kSkinFullId = 0x20;   // std::string (con él el juego arma el nombre de la textura)
constexpr std::uintptr_t kSkinImage = 0x80;    // SkinImage
constexpr std::uintptr_t kImageWidth = 0x04;
constexpr std::uintptr_t kImageHeight = 0x08;
constexpr std::uintptr_t kImageData = 0x20;
constexpr std::uintptr_t kImageSize = 0x28;

struct SharedPtr {  // std::shared_ptr de MSVC: el objeto y su "contador"
    void* object = nullptr;
    void* control = nullptr;
};
using CreatePacketFn = SharedPtr*(__fastcall*)(SharedPtr* out, int id);
using SendFn = void(__fastcall*)(void* sender, void* packet);
CreatePacketFn g_createPacket = nullptr;
std::atomic<bool> g_ready{false};

// La skin pendiente (la prepara Poll, la manda OnUiFrame).
std::mutex g_mutex;
std::vector<std::uint8_t> g_pixels;  // RGBA
unsigned g_width = 0;
unsigned g_height = 0;
std::string g_name;
std::atomic<bool> g_pending{false};

// Soltar un shared_ptr del juego como lo hace el juego: restar 1 al contador
// (+0x8); si llega a 0, destruir el objeto (función 0 del contador); después
// restar 1 a los "débiles" (+0xC) y, si llega a 0, borrar el contador (función 1).
void Release(SharedPtr& p) {
    if (p.control == nullptr) return;
    auto* control = static_cast<std::uint8_t*>(p.control);
    using Fn = void(__fastcall*)(void*);
    void** vtable = *reinterpret_cast<void***>(control);
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(control + 8)) == 0) {
        reinterpret_cast<Fn>(vtable[0])(control);
        if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(control + 12)) == 0) {
            reinterpret_cast<Fn>(vtable[1])(control);
        }
    }
    p = {};
}

bool CallCreate(SharedPtr* out, int id = kPlayerSkinPacketId) {
    __try {
        g_createPacket(out, id);
        return out->object != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallSend(SendFn fn, std::uintptr_t sender, std::uintptr_t packet) {
    __try {
        fn(reinterpret_cast<void*>(sender), reinterpret_cast<void*>(packet));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Un std::string corto (hasta 15 letras) escrito directamente en su lugar:
// los cortos viven ADENTRO del string, así no hay que pedir memoria (el
// juego y Luma usan memorias distintas: no se pueden mezclar).
void WriteShortString(std::uint8_t* where, const char* text) {
    const std::size_t len = std::min<std::size_t>(std::strlen(text), 15);
    std::memset(where, 0, 16);
    std::memcpy(where, text, len);
    const std::uint64_t size = len;
    const std::uint64_t cap = 15;
    std::memcpy(where + 0x10, &size, 8);
    std::memcpy(where + 0x18, &cap, 8);
}

// Lee un PNG a RGBA con WIC (el decodificador de imágenes de Windows).
bool DecodePng(const fs::path& file, std::vector<std::uint8_t>& rgba, unsigned& w, unsigned& h) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    bool ok = false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 1024 && h <= 1024 &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        rgba.resize(static_cast<std::size_t>(w) * h * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return ok;
}

nlohmann::json ReadJson(const fs::path& path) {
    std::ifstream file(path);
    return file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
}

}  // namespace

bool Init() {
    const ScanResult match = FindPattern(kCreatePacket);
    if (match.matches != 1) {
        log::Warn("SDK: no se encontró MinecraftPackets::createPacket ({} coincidencias): Equipar seguirá usando el "
                  "paquete del Vestidor.",
                  match.matches);
        return false;
    }
    g_createPacket = reinterpret_cast<CreatePacketFn>(match.address);
    g_ready.store(true);
    log::Info("SDK: crear paquetes en juego+0x{:X}.", match.address - GameBase());
    return true;
}

bool Available() {
    return g_ready.load(std::memory_order_relaxed);
}

bool CreatePacket(int id, PacketRef& out) {
    out = {};
    if (!g_ready.load(std::memory_order_relaxed)) return false;
    SharedPtr p;
    if (!CallCreate(&p, id)) return false;
    out.object = p.object;
    out.control = p.control;
    return true;
}

void ReleasePacket(PacketRef& packet) {
    SharedPtr p{packet.object, packet.control};
    Release(p);
    packet = {};
}

void Poll() {
    if (!g_ready.load(std::memory_order_relaxed)) return;
    // ¿Cambió la skin equipada? (active.json lo escriben el Closet y el launcher)
    static fs::file_time_type lastWrite{};
    static bool first = true;
    static ULONGLONG lastCheck = 0;
    if (GetTickCount64() - lastCheck < 500) return;
    lastCheck = GetTickCount64();
    const fs::path dir = paths::DataDir() / L"skins";
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(dir / L"active.json", ec);
    if (ec) return;
    if (first) {  // al cargar Luma no se aplica nada: solo cuando equipas
        first = false;
        lastWrite = t;
        return;
    }
    if (t == lastWrite) return;
    lastWrite = t;
    const nlohmann::json active = ReadJson(dir / L"active.json");
    const std::string id = active.is_object() ? active.value("id", std::string()) : std::string();
    const nlohmann::json lib = ReadJson(dir / L"library.json");
    if (id.empty() || !lib.is_object() || !lib.contains("skins") || !lib["skins"].is_array()) return;
    for (const nlohmann::json& s : lib["skins"]) {
        if (!s.is_object() || s.value("id", std::string()) != id) continue;
        static bool comReady = false;
        if (!comReady) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comReady = true;
        }
        std::vector<std::uint8_t> rgba;
        unsigned w = 0, h = 0;
        if (!DecodePng(dir / fs::path(s.value("file", std::string())), rgba, w, h)) {
            log::Warn("Skin al instante: no se pudo leer el PNG de la skin equipada.");
            return;
        }
        std::lock_guard lock(g_mutex);
        g_pixels = std::move(rgba);
        g_width = w;
        g_height = h;
        g_name = s.value("name", std::string());
        g_pending.store(true);
        return;
    }
}

void OnUiFrame() {
    if (!g_pending.load(std::memory_order_relaxed) || !g_ready.load(std::memory_order_relaxed)) return;
    g_pending.store(false);
    const std::uintptr_t player = sdk::player::LocalPlayer();
    if (player == 0) {
        notify::Push(notify::Kind::Warning, Tr("Instant skin: join a world first"));
        return;
    }
    // 1. Lo que necesitamos del jugador: su skin actual, su PacketSender y su UUID.
    std::uintptr_t shared = 0, skin = 0, sender = 0, senderVt = 0, sendFn = 0;
    if (!SafeRead(player + kPlayerSkin, shared) || !LooksLikePointer(shared) || !SafeRead(shared, skin) || !LooksLikePointer(skin) ||
        !SafeRead(player + kPlayerPacketSender, sender) || !LooksLikePointer(sender) || !SafeRead(sender, senderVt) ||
        !LooksLikePointer(senderVt) || !SafeRead(senderVt + kSendToServerIndex * 8, sendFn) || sendFn < GameBase()) {
        log::Warn("Skin al instante: no se pudo leer la skin o el PacketSender del jugador.");
        return;
    }
    char ownName[64]{};
    std::uint8_t uuid[16]{};
    if (!SafeReadString(player + sdk::offsets::kPlayerName, ownName, sizeof(ownName)) || !sdk::players::UuidOf(ownName, uuid)) {
        log::Warn("Skin al instante: no se encontró tu UUID en la lista de jugadores.");
        return;
    }
    std::uint32_t w = 0, h = 0;
    std::uint64_t size = 0;
    if (!SafeRead(skin + kSkinImage + kImageWidth, w) || !SafeRead(skin + kSkinImage + kImageHeight, h) ||
        !SafeRead(skin + kSkinImage + kImageSize, size)) {
        log::Warn("Skin al instante: no se pudo leer la imagen de la skin actual.");
        return;
    }
    std::lock_guard lock(g_mutex);
    if (g_width != w || g_height != h || g_pixels.size() != static_cast<std::size_t>(w) * h * 4) {
        notify::Push(notify::Kind::Warning, Tr("Instant skin: that skin is a different size from the current one"));
        log::Info("Skin al instante: tamaños distintos (actual {}x{}, nueva {}x{}).", w, h, g_width, g_height);
        return;
    }

    // 2. Un paquete de skin nuevo (lo crea el juego, con su propia skin vacía adentro).
    SharedPtr packet;
    if (!CallCreate(&packet)) {
        log::Warn("Skin al instante: no se pudo crear el paquete de skin.");
        return;
    }
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(packet.object);
    std::uintptr_t packetSkin = 0;
    if (!SafeRead(p + kPacketSkin, packetSkin) || !LooksLikePointer(packetSkin)) {
        Release(packet);
        log::Warn("Skin al instante: el paquete no trae su skin.");
        return;
    }
    // 3. SOLO MIENTRAS SE MANDA: la skin del paquete = una copia "superficial"
    //    de la tuya (mismos datos, sin duplicar nada), con tus píxeles nuevos y
    //    un identificador nuevo (para que el juego cree una textura nueva).
    //    Antes de soltar el paquete le devolvemos su contenido original: así
    //    nadie libera dos veces la misma memoria.
    std::uint8_t original[kSkinSize];
    std::uint8_t temp[kSkinSize];
    if (!SafeCopy(packetSkin, original, kSkinSize) || !SafeCopy(skin, temp, kSkinSize)) {
        Release(packet);
        return;
    }
    static std::uint32_t counter = 0;
    char id[16];
    std::snprintf(id, sizeof(id), "luma.%08X", (GetTickCount() ^ (++counter * 2654435761u)));
    WriteShortString(temp + kSkinId, id);
    WriteShortString(temp + kSkinFullId, id);
    const std::uintptr_t pixels = reinterpret_cast<std::uintptr_t>(g_pixels.data());
    const std::uint64_t pixelBytes = g_pixels.size();
    std::memcpy(temp + kSkinImage + kImageData, &pixels, 8);
    std::memcpy(temp + kSkinImage + kImageSize, &pixelBytes, 8);
    std::uint8_t names[0x40];
    SafeCopy(p + kPacketNewName, names, sizeof(names));  // (los nombres vacíos del paquete, para devolverlos)
    std::uint8_t newName[0x20], oldName[0x20];
    WriteShortString(newName, "luma");
    WriteShortString(oldName, "");
    bool sent = false;
    if (SafeWrite(p + kPacketUuid, uuid, 16) && SafeWrite(packetSkin, temp, kSkinSize) && SafeWrite(p + kPacketNewName, newName, 0x20) &&
        SafeWrite(p + kPacketOldName, oldName, 0x20)) {
        sent = CallSend(reinterpret_cast<SendFn>(sendFn), sender, p);
    }
    // 4. Devolver todo y soltar el paquete.
    SafeWrite(packetSkin, original, kSkinSize);
    SafeWrite(p + kPacketNewName, names, sizeof(names));
    Release(packet);
    if (!sent) {
        log::Error("Skin al instante: no se pudo mandar el paquete (excepción atrapada).");
        return;
    }
    log::Info("Skin al instante: mandado el paquete de skin \"{}\" ({}x{}, id {}).", g_name, w, h, id);
    notify::Push(notify::Kind::Success, Tr("Skin applied"));
}

}  // namespace luma::sdk::skinapply
