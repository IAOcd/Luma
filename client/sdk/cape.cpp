// =============================================================================
//  cape.cpp - Capas de Luma (ver cape.h)
// =============================================================================
#include "sdk/cape.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/logger.h"
#include "sdk/memory.h"
#include "sdk/network.h"
#include "sdk/offsets.h"
#include "sdk/player.h"
#include "sdk/players.h"
#include "sdk/scanner.h"
#include "sdk/skin_apply.h"

namespace luma::sdk::cape {

namespace {

// --- El paquete de skin (PlayerSkinPacket, número 93; ver sdk/skin_apply) -----------
constexpr int kPlayerSkinPacketId = 93;
constexpr std::uintptr_t kPacketUuid = 0x30;     // mce::UUID (16 bytes)
constexpr std::uintptr_t kPacketSkin = 0x40;     // shared_ptr<SerializedSkin> (puntero en +0x40)
constexpr std::uintptr_t kPacketNewName = 0x50;  // std::string
constexpr std::uintptr_t kPacketOldName = 0x70;  // std::string
constexpr std::uintptr_t kPlayerPacketSender = 0x820;
constexpr std::size_t kSendToServerIndex = 4;

// --- La skin (SerializedSkin) ---------------------------------------------------------
constexpr std::uintptr_t kPlayerSkin = 0xAD8;  // Player -> shared_ptr<SerializedSkin>*
constexpr std::size_t kSkinSize = 0x210;       // (lo reserva el constructor del paquete)
constexpr std::uintptr_t kSkinId = 0x00;
constexpr std::uintptr_t kSkinFullId = 0x20;
constexpr std::uintptr_t kSkinImage = 0x80;
constexpr std::uintptr_t kCapeImage = 0xB0;
constexpr std::uintptr_t kCapeId = 0x158;
// Una imagen: { formato, ancho, alto, profundidad, uso, relleno, { borrador, píxeles, tamaño } }
constexpr std::uintptr_t kImageWidth = 0x04;
constexpr std::uintptr_t kImageHeight = 0x08;
constexpr std::uintptr_t kImageData = 0x20;
constexpr std::uintptr_t kImageSize = 0x28;
constexpr std::size_t kImageHeader = 0x18;  // formato, ancho, alto, profundidad, uso
// Marcas de 1 byte al final de la skin: +0x1E8 "confiable", +0x1EA "persona",
// +0x1EB "capa Persona sobre skin clásica": la llevan las capas del Vestidor
// (lo vimos en el log: aparece al ponerte una). Con ella encendida el juego
// dibuja la capa del Vestidor y no la imagen de la capa: para que se vea la
// de Luma hay que mandarla apagada.
constexpr std::uintptr_t kSkinFlags = 0x1E8;
constexpr std::uintptr_t kPersonaCapeOnClassic = 0x1EB;

using SendFn = void(__fastcall*)(void* sender, void* packet);

std::atomic<bool> g_ready{false};

// Lo que pidió core/cosmetics (hilo principal) y se manda en OnUiFrame.
std::mutex g_mutex;
std::vector<std::uint8_t> g_wantedRgba;
unsigned g_wantedW = 0, g_wantedH = 0;
std::string g_wantedId;
std::atomic<bool> g_pending{false};
// Tu capa del juego, para devolvértela si eliges "ninguna" (por jugador).
std::uintptr_t g_originalFor = 0;
bool g_haveOriginal = false;
std::vector<std::uint8_t> g_originalRgba;
unsigned g_originalW = 0, g_originalH = 0;
std::string g_originalId;
std::uint8_t g_originalPersonaCape = 0;  // su marca "capa del Vestidor"


bool LocalSkin(std::uintptr_t player, std::uintptr_t& skin) {
    std::uintptr_t ref = 0;
    return player != 0 && SafeRead(player + kPlayerSkin, ref) && LooksLikePointer(ref) && SafeRead(ref, skin) &&
           LooksLikePointer(skin);
}

bool ReadCape(std::uintptr_t skin, std::vector<std::uint8_t>& rgba, unsigned& outW, unsigned& outH, std::string& id) {
    char text[96]{};
    std::uint32_t w = 0, h = 0;
    std::uintptr_t data = 0;
    std::uint64_t size = 0;
    if (!SafeReadString(skin + kCapeId, text, sizeof(text)) || !SafeRead(skin + kCapeImage + kImageWidth, w) ||
        !SafeRead(skin + kCapeImage + kImageHeight, h)) {
        return false;
    }
    id = text;
    rgba.clear();
    outW = outH = 0;
    if (w == 0 || h == 0) return true;  // sin capa
    if (!ValidSize(w, h) || !SafeRead(skin + kCapeImage + kImageData, data) ||
        !SafeRead(skin + kCapeImage + kImageSize, size) || size != static_cast<std::uint64_t>(w) * h * 4 ||
        !LooksLikePointer(data)) {
        return false;
    }
    rgba.resize(static_cast<std::size_t>(size));
    outW = w;
    outH = h;
    return SafeCopy(data, rgba.data(), rgba.size());
}

// Un std::string de MSVC que APUNTA a un texto nuestro (para la copia
// temporal de la skin: nadie lo libera porque después se restaura todo).
// Hasta 15 letras va dentro del objeto.
void TempString(std::uint8_t* where, const std::string& text) {
    std::memset(where, 0, 0x20);
    const std::uint64_t size = text.size();
    std::uint64_t cap = 15;
    if (size <= 15) {
        std::memcpy(where, text.data(), text.size());
    } else {
        cap = size;
        const auto ptr = reinterpret_cast<std::uintptr_t>(text.c_str());
        std::memcpy(where, &ptr, 8);
    }
    std::memcpy(where + 0x10, &size, 8);
    std::memcpy(where + 0x18, &cap, 8);
}

bool CallSend(SendFn fn, std::uintptr_t sender, std::uintptr_t packet) {
    __try {
        fn(reinterpret_cast<void*>(sender), reinterpret_cast<void*>(packet));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Manda tu skin con esta capa a tu servidor (mundo local). Hilo del juego.
bool SendCape(const std::vector<std::uint8_t>& rgba, unsigned capeW, unsigned capeH, const std::string& capeId,
              std::uint8_t personaCape) {
    const std::uintptr_t player = player::LocalPlayer();
    std::uintptr_t skin = 0, sender = 0, senderVt = 0, sendFn = 0;
    char own[64]{};
    std::uint8_t uuid[16]{};
    if (!LocalSkin(player, skin) || !SafeRead(player + kPlayerPacketSender, sender) || !LooksLikePointer(sender) ||
        !SafeRead(sender, senderVt) || !LooksLikePointer(senderVt) ||
        !SafeRead(senderVt + kSendToServerIndex * 8, sendFn) || sendFn < GameBase() ||
        !SafeReadString(player + offsets::kPlayerName, own, sizeof(own)) || !players::UuidOf(own, uuid)) {
        log::Warn("Capas: no se pudo leer tu skin, tu UUID o el PacketSender.");
        return false;
    }
    skinapply::PacketRef packet;
    if (!skinapply::CreatePacket(kPlayerSkinPacketId, packet)) {
        log::Warn("Capas: no se pudo crear el paquete de skin.");
        return false;
    }
    const auto p = reinterpret_cast<std::uintptr_t>(packet.object);
    std::uintptr_t packetSkin = 0;
    std::uint8_t original[kSkinSize];
    std::uint8_t temp[kSkinSize];
    if (!SafeRead(p + kPacketSkin, packetSkin) || !LooksLikePointer(packetSkin) ||
        !SafeCopy(packetSkin, original, kSkinSize) || !SafeCopy(skin, temp, kSkinSize)) {
        skinapply::ReleasePacket(packet);
        log::Warn("Capas: no se pudo leer tu skin.");
        return false;
    }
    // Copia "superficial" de tu skin (comparte textos y píxeles con la de
    // verdad, solo mientras se manda) con la capa cambiada y un id nuevo
    // (con el mismo id el juego reusaría las texturas viejas).
    static std::uint32_t counter = 0;
    char idText[16];
    std::snprintf(idText, sizeof(idText), "lc.%08X", GetTickCount() ^ (++counter * 2654435761u));
    const std::string newId = idText;
    TempString(temp + kSkinId, newId);
    TempString(temp + kSkinFullId, newId);
    TempString(temp + kCapeId, capeId);
    std::uint32_t oldW = 0;
    std::memcpy(&oldW, temp + kCapeImage + kImageWidth, 4);
    if (oldW == 0 && !rgba.empty()) {
        // No tenías capa: copiar de la imagen de la skin el formato (RGBA).
        std::memcpy(temp + kCapeImage, temp + kSkinImage, kImageHeader);
    }
    const std::uint32_t w = rgba.empty() ? 0 : capeW, h = rgba.empty() ? 0 : capeH;
    const auto pixels = rgba.empty() ? std::uintptr_t{0} : reinterpret_cast<std::uintptr_t>(rgba.data());
    const std::uint64_t bytes = rgba.size();
    std::memcpy(temp + kCapeImage + kImageWidth, &w, 4);
    std::memcpy(temp + kCapeImage + kImageHeight, &h, 4);
    std::memcpy(temp + kCapeImage + kImageData, &pixels, 8);
    std::memcpy(temp + kCapeImage + kImageSize, &bytes, 8);
    log::Info("Capas: marcas de la skin (+0x1E8): {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} -> capa del Vestidor {}",
              temp[kSkinFlags], temp[kSkinFlags + 1], temp[kSkinFlags + 2], temp[kSkinFlags + 3],
              temp[kSkinFlags + 4], temp[kSkinFlags + 5], personaCape);
    temp[kPersonaCapeOnClassic] = personaCape;

    std::uint8_t names[0x40];
    SafeCopy(p + kPacketNewName, names, sizeof(names));
    std::uint8_t newName[0x20], oldName[0x20];
    TempString(newName, "luma");
    TempString(oldName, "");
    bool sent = false;
    if (SafeWrite(p + kPacketUuid, uuid, 16) && SafeWrite(packetSkin, temp, kSkinSize) &&
        SafeWrite(p + kPacketNewName, newName, 0x20) && SafeWrite(p + kPacketOldName, oldName, 0x20)) {
        // sendToServer escribe el paquete en bytes en el momento: después ya
        // podemos devolver todo como estaba.
        sent = CallSend(reinterpret_cast<SendFn>(sendFn), sender, p);
    }
    SafeWrite(packetSkin, original, kSkinSize);
    SafeWrite(p + kPacketNewName, names, sizeof(names));
    skinapply::ReleasePacket(packet);
    if (!sent) {
        log::Error("Capas: no se pudo mandar el paquete (excepción atrapada).");
        return false;
    }
    log::Info("Capas: capa \"{}\" mandada a tu mundo (skin {}).", capeId.empty() ? "(ninguna)" : capeId, newId);
    return true;
}

}  // namespace

bool Init() {
    if (!skinapply::Available()) {
        log::Warn("Capas: sin MinecraftPackets::createPacket no se pueden poner capas.");
        return false;
    }
    g_ready.store(true);
    log::Info("Capas: listo (mundos locales).");
    return true;
}

bool Available() {
    return g_ready.load(std::memory_order_relaxed);
}

void SetLocalCape(const std::vector<std::uint8_t>& rgba, unsigned width, unsigned height, const std::string& capeId) {
    if (!rgba.empty() && (!ValidSize(width, height) || rgba.size() != static_cast<std::size_t>(width) * height * 4)) return;
    std::lock_guard lock(g_mutex);
    g_wantedRgba = rgba;
    g_wantedW = width;
    g_wantedH = height;
    g_wantedId = capeId;
    g_pending.store(true);
}

void OnUiFrame() {
    if (!g_ready.load(std::memory_order_relaxed) || !g_pending.load(std::memory_order_relaxed)) return;
    const std::uintptr_t player = player::LocalPlayer();
    if (player == 0) return;  // (todavía no hay jugador: esperar)
    if (network::OnServer()) {
        // Paso 2 (todavía no): en un servidor no se manda nada (lo verían todos).
        g_pending.store(false);
        log::Info("Capas: en servidores todavía no (se pone en tus mundos locales).");
        return;
    }
    std::vector<std::uint8_t> rgba;
    unsigned w = 0, h = 0;
    std::string id;
    {
        std::lock_guard lock(g_mutex);
        rgba = g_wantedRgba;
        w = g_wantedW;
        h = g_wantedH;
        id = g_wantedId;
        g_pending.store(false);
    }
    // Tu capa del juego, antes de la primera de Luma (para devolvértela).
    std::uintptr_t skin = 0;
    if (g_originalFor != player && LocalSkin(player, skin)) {
        std::vector<std::uint8_t> origRgba;
        unsigned origW = 0, origH = 0;
        std::string origId;
        if (ReadCape(skin, origRgba, origW, origH, origId)) {
            g_originalFor = player;
            g_haveOriginal = origId.rfind("luma_", 0) != 0;
            if (g_haveOriginal) {
                g_originalRgba = std::move(origRgba);
                g_originalW = origW;
                g_originalH = origH;
                g_originalId = std::move(origId);
                g_originalPersonaCape = 0;
                SafeRead(skin + kPersonaCapeOnClassic, g_originalPersonaCape);
                if (g_originalPersonaCape > 1) g_originalPersonaCape = 0;
            }
        }
    }
    if (id.empty()) {
        // "Ninguna": solo hace falta si llevas una de Luma.
        std::vector<std::uint8_t> nowRgba;
        unsigned nowW = 0, nowH = 0;
        std::string nowId;
        if (!LocalSkin(player, skin) || !ReadCape(skin, nowRgba, nowW, nowH, nowId) || nowId.rfind("luma_", 0) != 0) return;
        // Devolver la del juego (si la tenías).
        rgba = g_haveOriginal ? g_originalRgba : std::vector<std::uint8_t>();
        w = g_originalW;
        h = g_originalH;
        id = g_haveOriginal ? g_originalId : std::string();
    }
    // Una capa de Luma: la marca del Vestidor apagada. "Ninguna": la de antes.
    const std::uint8_t personaCape = id.rfind("luma_", 0) == 0 ? 0 : (g_haveOriginal ? g_originalPersonaCape : 0);
    SendCape(rgba, w, h, id, personaCape);
}

}  // namespace luma::sdk::cape
