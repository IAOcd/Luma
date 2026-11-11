// =============================================================================
//  players.cpp - Lista de jugadores y servidor (ver players.h)
// =============================================================================
#include "sdk/players.h"

#include <windows.h>

#include <cstring>

#include "core/logger.h"
#include "sdk/memory.h"
#include "sdk/player.h"
#include "sdk/scanner.h"

namespace luma::sdk::players {

namespace {

// Comprobado en 1.26.63 leyendo la memoria del juego (Luma cargado, mundo local):
// el jugador guarda un puntero a su Level en +0x200 (Actor::mLevel).
constexpr std::uintptr_t kPlayerLevel = 0x200;
// Level -> LevelData (+0x90) -> nombre del mundo (std::string en +0x2B0).
constexpr std::uintptr_t kLevelData = 0x90;
constexpr std::uintptr_t kLevelDataName = 0x2B0;
constexpr std::uintptr_t kLevelPlayerList = 0x4E8;
// std::unordered_map de Microsoft: +0x08 la lista de nodos (cabeza), +0x10 cuántos.
// Cada nodo: +0x00 siguiente, +0x08 anterior, +0x10 el valor (UUID de 16 bytes + PlayerListEntry).
constexpr std::uintptr_t kMapListHead = 0x08;
constexpr std::uintptr_t kMapSize = 0x10;
constexpr std::uintptr_t kNodeValue = 0x10;
constexpr std::uintptr_t kUuidSize = 0x10;
constexpr std::uintptr_t kEntryName = 0x18;
constexpr std::uintptr_t kEntrySkin = 0xA0;
// La skin (igual que en sdk/skin.cpp).
constexpr std::uintptr_t kSkinId = 0x00;
constexpr std::uintptr_t kSkinImage = 0x80;

constexpr std::uintptr_t kPlayerPacketSender = 0x820;
constexpr std::uintptr_t kSenderNetworkSystem = 0x20;
constexpr std::uintptr_t kSystemConnector = 0x120;
constexpr std::uintptr_t kConnectorSessionOwner = 0x88;
constexpr std::uintptr_t kConnectorNetherNet = 0x90;
constexpr std::uintptr_t kConnectorRakNet = 0x98;
constexpr std::uintptr_t kInfoHost = 0x08;
constexpr std::uintptr_t kInfoUrl = 0x28;
constexpr std::uintptr_t kInfoPort = 0x8C;

bool InsideGame(std::uintptr_t address);

std::uintptr_t LevelPtr() {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t level = 0, vtable = 0;
    // El Level tiene que ser un objeto del juego (su vtable está dentro del .exe).
    if (player == 0 || !SafeRead(player + kPlayerLevel, level) || !LooksLikePointer(level) ||
        !SafeRead(level, vtable) || !InsideGame(vtable)) {
        return 0;
    }
    return level;
}

// Saca los códigos de color de Minecraft ("§a", "§l"...) de un nombre.
std::string StripColors(const char* text) {
    std::string out;
    for (const char* c = text; *c != 0; ++c) {
        // "§" en UTF-8 son dos bytes: C2 A7. Después viene la letra del código.
        if (static_cast<unsigned char>(c[0]) == 0xC2 && static_cast<unsigned char>(c[1]) == 0xA7) {
            c += c[2] != 0 ? 2 : 1;
            continue;
        }
        out += *c;
    }
    return out;
}

// La cara de 8x8 de la skin de un jugador (frente de la cabeza + segunda capa).
bool ReadFace(std::uintptr_t skinRef, Player& p) {
    std::uintptr_t impl = 0;
    if (!SafeRead(skinRef, impl) || !LooksLikePointer(impl)) return false;
    char id[96]{};
    SafeReadString(impl + kSkinId, id, sizeof(id));
    p.skinId = id;
    const std::uintptr_t image = impl + kSkinImage;
    std::uint32_t w = 0, h = 0;
    std::uintptr_t data = 0;
    std::uint64_t size = 0;
    if (!SafeRead(image + 0x04, w) || !SafeRead(image + 0x08, h) || !SafeRead(image + 0x20, data) ||
        !SafeRead(image + 0x28, size) || w < 64 || w > 1024 || w % 64 != 0 || (h != w && h * 2 != w) ||
        size < static_cast<std::uint64_t>(w) * h * 4 || !LooksLikePointer(data & ~std::uintptr_t{0xF})) {
        return false;
    }
    const std::uint32_t k = w / 64;
    for (int y = 0; y < 8; ++y) {
        // Una fila de la cabeza y una de la segunda capa (copiamos solo lo necesario).
        std::uint8_t base[8 * 4 * 16];  // hasta skins de 1024 (k = 16)
        std::uint8_t hat[8 * 4 * 16];
        const std::uintptr_t row = data + (static_cast<std::uintptr_t>(8 + y) * k * w) * 4;
        if (!SafeCopy(row + (8 * k) * 4, base, 8 * k * 4) || !SafeCopy(row + (40 * k) * 4, hat, 8 * k * 4)) return false;
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t* b = base + (x * k) * 4;
            const std::uint8_t* t = hat + (x * k) * 4;
            const float a = t[3] / 255.0f;
            std::uint8_t* o = p.face + (y * 8 + x) * 4;
            for (int i = 0; i < 3; ++i) o[i] = static_cast<std::uint8_t>(b[i] * (1.0f - a) + t[i] * a);
            o[3] = 255;
        }
    }
    p.hasFace = true;
    return true;
}

bool InsideGame(std::uintptr_t address) {
    const std::uintptr_t base = GameBase();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
}

using GetInfoFn = std::uintptr_t(__fastcall*)(void* connector);

std::uintptr_t CallInfo(GetInfoFn fn, void* connector) {
    __try {
        return fn(connector);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

bool Read(std::vector<Player>& out, bool withFaces, std::size_t max) {
    out.clear();
    const std::uintptr_t level = LevelPtr();
    std::uintptr_t map = 0, head = 0;
    std::uint64_t count = 0;
    if (level == 0 || !SafeRead(level + kLevelPlayerList, map) || !LooksLikePointer(map) ||
        !SafeRead(map + kMapListHead, head) || !LooksLikePointer(head) || !SafeRead(map + kMapSize, count) || count > 5000) {
        return false;
    }
    std::uintptr_t node = 0;
    SafeRead(head, node);  // el primero (la cabeza es un nodo "vacío")
    for (std::size_t i = 0; i < count && i < max && node != head && LooksLikePointer(node); ++i) {
        const std::uintptr_t entry = node + kNodeValue + kUuidSize;
        char name[64]{};
        if (SafeReadString(entry + kEntryName, name, sizeof(name)) && name[0] != 0) {
            Player p;
            p.name = StripColors(name);
            if (withFaces) {
                ReadFace(entry + kEntrySkin, p);
            } else {
                // solo el id de la skin (barato), para saber si hay que releerla
                std::uintptr_t impl = 0;
                char id[96]{};
                if (SafeRead(entry + kEntrySkin, impl) && LooksLikePointer(impl) && SafeReadString(impl + kSkinId, id, sizeof(id))) {
                    p.skinId = id;
                }
            }
            out.push_back(std::move(p));
        }
        if (!SafeRead(node, node)) break;  // siguiente
    }
    return true;
}

bool UuidOf(const std::string& name, std::uint8_t out[16]) {
    const std::uintptr_t level = LevelPtr();
    std::uintptr_t map = 0, head = 0, node = 0;
    std::uint64_t count = 0;
    if (level == 0 || !SafeRead(level + kLevelPlayerList, map) || !LooksLikePointer(map) ||
        !SafeRead(map + kMapListHead, head) || !LooksLikePointer(head) || !SafeRead(map + kMapSize, count) || count > 5000) {
        return false;
    }
    SafeRead(head, node);
    for (std::size_t i = 0; i < count && node != head && LooksLikePointer(node); ++i) {
        char text[64]{};
        if (SafeReadString(node + kNodeValue + kUuidSize + kEntryName, text, sizeof(text)) && StripColors(text) == name) {
            return SafeCopy(node + kNodeValue, out, kUuidSize);  // la clave del mapa = el UUID
        }
        if (!SafeRead(node, node)) break;
    }
    return false;
}

bool WorldName(std::string& out) {
    const std::uintptr_t level = LevelPtr();
    std::uintptr_t data = 0;
    char name[128]{};
    if (level == 0 || !SafeRead(level + kLevelData, data) || !LooksLikePointer(data) ||
        !SafeReadString(data + kLevelDataName, name, sizeof(name)) || name[0] == 0) {
        return false;
    }
    out = StripColors(name);
    return true;
}

bool ReadServer(Server& out) {
    // Si algo falla, se anota UNA vez en el log en qué paso (para poder arreglarlo
    // tras una actualización del juego sin adivinar).
    static int s_loggedStep = 0;
    auto fail = [](int step) {
        if (s_loggedStep != step) {
            s_loggedStep = step;
            log::Info("Servidor: no se pudo leer la dirección (paso {}).", step);
        }
        return false;
    };
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t sender = 0, system = 0, composite = 0, sessionOwner = 0, sessionInfo = 0;
    if (player == 0 || !SafeRead(player + kPlayerPacketSender, sender) || !LooksLikePointer(sender) ||
        !SafeRead(sender + kSenderNetworkSystem, system) || !LooksLikePointer(system)) {
        return fail(1);
    }
    if (!SafeRead(system + kSystemConnector, composite) || !LooksLikePointer(composite) ||
        !SafeRead(composite + kConnectorSessionOwner, sessionOwner) || !LooksLikePointer(sessionOwner)) {
        return fail(2);
    }
    // ¿Conexión NetherNet (mundos de amigos, Realms) o RakNet (servidores)?
    int kind = 0;
    const bool netherNet = SafeRead(sessionOwner + 0x18, sessionInfo) && LooksLikePointer(sessionInfo) &&
                           SafeRead(sessionInfo + 0x18, kind) && kind == 2;
    std::uintptr_t connector = 0, vtable = 0, fn = 0;
    if (!SafeRead(composite + (netherNet ? kConnectorNetherNet : kConnectorRakNet), connector) ||
        !LooksLikePointer(connector) || !SafeRead(connector, vtable) || !InsideGame(vtable) ||
        !SafeRead(vtable + 3 * 8, fn) || !InsideGame(fn)) {
        return fail(3);
    }
    const std::uintptr_t info = CallInfo(reinterpret_cast<GetInfoFn>(fn), reinterpret_cast<void*>(connector));
    if (!LooksLikePointer(info)) return fail(netherNet ? 40 : 4);
    char url[128]{};
    char host[128]{};
    SafeReadString(info + kInfoUrl, url, sizeof(url));
    SafeReadString(info + kInfoHost, host, sizeof(host));
    SafeRead(info + kInfoPort, out.port);
    out.address = url[0] != 0 ? url : host;
    if (out.address.empty()) return fail(netherNet ? 50 : 5);
    if (s_loggedStep != -1) {
        s_loggedStep = -1;
        log::Info("Servidor: dirección {} (puerto {}, {}).", out.address, out.port, netherNet ? "NetherNet" : "RakNet");
    }
    return true;
}

}  // namespace luma::sdk::players
