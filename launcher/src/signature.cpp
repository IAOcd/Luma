// =============================================================================
//  signature.cpp - Firma ECDSA P-256 y SHA-256 con BCrypt (ver signature.h)
// =============================================================================
#include "signature.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace luma::launcher::signature {

namespace {

// La clave PÚBLICA de Luma: el punto (X, Y) de la curva P-256, 32 bytes cada uno.
// (La privada correspondiente nunca está en el código.) Si algún día se cambia
// de clave, los launchers viejos dejarían de aceptar actualizaciones.
constexpr std::uint8_t kPublicKey[64] = {
    // X
    0x5c, 0xb4, 0x46, 0x09, 0x94, 0xbb, 0x11, 0x78, 0x9d, 0xdf, 0xd5, 0x6f, 0xca, 0x03, 0xcf, 0x4d,
    0x0a, 0x04, 0xcf, 0x63, 0xee, 0x7d, 0x4a, 0x52, 0x8a, 0x50, 0x00, 0x13, 0x12, 0x4b, 0x7f, 0x08,
    // Y
    0x12, 0x5e, 0x1f, 0x37, 0x47, 0x08, 0x14, 0x99, 0xbb, 0xe8, 0xd3, 0x4a, 0x0c, 0x69, 0x99, 0x45,
    0x4b, 0x91, 0x6e, 0x2b, 0x95, 0xb1, 0xc6, 0x4c, 0x7a, 0x22, 0x48, 0x22, 0x3f, 0x27, 0xd3, 0x9f,
};

bool Sha256(const void* data, std::size_t size, std::uint8_t out[32]) {
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                     static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), out, 32));
}

}  // namespace

bool Verify(const std::string& data, const std::string& sig) {
    if (sig.size() != 64) {
        return false;
    }
    std::uint8_t hash[32];
    if (!Sha256(data.data(), data.size(), hash)) {
        return false;
    }
    // Formato que pide BCrypt para una clave pública de curva elíptica:
    // encabezado {"ECS1", 32} y después X e Y.
    std::uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), kPublicKey, 64);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                            sizeof(blob), 0))) {
        return false;
    }
    const NTSTATUS status = BCryptVerifySignature(key, nullptr, hash, sizeof(hash),
                                                  reinterpret_cast<PUCHAR>(const_cast<char*>(sig.data())), 64, 0);
    BCryptDestroyKey(key);
    return BCRYPT_SUCCESS(status);
}

std::string Sha256File(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) {
        return {};
    }
    std::vector<char> buffer(64 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = in.gcount();
        if (n > 0) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(n), 0);
        }
    }
    std::uint8_t digest[32];
    const bool ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    BCryptDestroyHash(hash);
    if (!ok) {
        return {};
    }
    char hex[65];
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, 64);
}

}  // namespace luma::launcher::signature
