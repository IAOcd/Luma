// =============================================================================
//  skin.cpp - Leer la skin del jugador local (ver skin.h)
// =============================================================================
#include "sdk/skin.h"

#include "sdk/memory.h"
#include "sdk/player.h"

namespace luma::sdk::skin {

namespace {

constexpr std::uintptr_t kPlayerSkin = 0xAD8;      // Player -> SerializedSkinRef*
constexpr std::uintptr_t kSkinId = 0x00;           // std::string
constexpr std::uintptr_t kSkinImage = 0x80;        // SkinImage
constexpr std::uintptr_t kImageWidth = 0x04;       // uint32
constexpr std::uintptr_t kImageHeight = 0x08;      // uint32
constexpr std::uintptr_t kImageBlobData = 0x20;    // puntero a los píxeles (después del "borrador" en +0x18)
constexpr std::uintptr_t kImageBlobSize = 0x28;    // size_t

}  // namespace

bool ReadLocal(Image& out) {
    const std::uintptr_t player = player::LocalPlayer();
    std::uintptr_t ref = 0;
    std::uintptr_t impl = 0;
    if (player == 0 || !SafeRead(player + kPlayerSkin, ref) || !LooksLikePointer(ref) || !SafeRead(ref, impl) ||
        !LooksLikePointer(impl)) {
        return false;
    }
    const std::uintptr_t image = impl + kSkinImage;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::uintptr_t data = 0;
    std::uint64_t size = 0;
    if (!SafeRead(image + kImageWidth, w) || !SafeRead(image + kImageHeight, h) || !SafeRead(image + kImageBlobData, data) ||
        !SafeRead(image + kImageBlobSize, size)) {
        return false;
    }
    // Tamaños de skin razonables: 64x32 (vieja), 64x64, 128x128, 256x256...
    if (w < 32 || h < 32 || w > 1024 || h > 1024 || (w % 32) != 0 || (h % 32) != 0 ||
        size < static_cast<std::uint64_t>(w) * h * 4 || !LooksLikePointer(data & ~std::uintptr_t{0xF})) {
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (!SafeCopy(data, out.rgba.data(), out.rgba.size())) {
        return false;
    }
    char id[128]{};
    SafeReadString(impl + kSkinId, id, sizeof(id));
    out.id = id;
    return true;
}

}  // namespace luma::sdk::skin
