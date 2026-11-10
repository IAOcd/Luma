// =============================================================================
//  entity.cpp - Implementación de la lectura de componentes (ver entity.h)
// =============================================================================
#include "sdk/entity.h"

#include "sdk/memory.h"
#include "sdk/offsets.h"

namespace luma::sdk::entity {

namespace o = offsets;

std::uintptr_t Storage(std::uintptr_t registry, std::uint32_t typeHash) {
    // Buscar la piscina del tipo en la tabla hash.
    std::uintptr_t sparse[2]{};   // inicio y final del vector de cubetas
    std::uintptr_t packed = 0;    // inicio del vector de nodos
    if (!SafeRead(registry + o::kRegistryPoolsSparse, sparse) || !SafeRead(registry + o::kRegistryPoolsPacked, packed) ||
        !LooksLikePointer(sparse[0]) || !LooksLikePointer(packed) || sparse[1] <= sparse[0]) {
        return 0;
    }
    const std::uintptr_t buckets = (sparse[1] - sparse[0]) / 8;
    if ((buckets & (buckets - 1)) != 0 || buckets > (1u << 20)) {
        return 0;  // siempre es potencia de 2: si no, el formato no es el esperado
    }
    std::uint64_t index = 0;
    if (!SafeRead(sparse[0] + (typeHash & (buckets - 1)) * 8, index)) {
        return 0;
    }
    for (int step = 0; step < 64 && index < 100000; ++step) {
        const std::uintptr_t node = packed + static_cast<std::uintptr_t>(index) * o::kPoolNodeSize;
        std::uint32_t key = 0;
        if (!SafeRead(node + o::kPoolNodeKey, key)) {
            return 0;
        }
        if (key == typeHash) {
            std::uintptr_t storage = 0;
            SafeRead(node + o::kPoolNodeStorage, storage);
            return LooksLikePointer(storage) ? storage : 0;
        }
        if (!SafeRead(node + o::kPoolNodeNext, index)) {
            return 0;
        }
    }
    return 0;
}

std::uintptr_t ComponentOf(std::uintptr_t storage, std::uint32_t entityId, std::uintptr_t size) {
    if (storage == 0) {
        return 0;
    }
    // En qué posición está la entidad (arreglo "sparse", por páginas de 2048).
    const std::uint32_t entityIndex = entityId & o::kEntityMask;
    std::uintptr_t sparsePages[2]{};
    std::uintptr_t sparsePage = 0;
    std::uint32_t position = 0;
    if (!SafeRead(storage + o::kStorageSparse, sparsePages) || !LooksLikePointer(sparsePages[0]) ||
        (entityIndex / o::kSparsePage) >= (sparsePages[1] - sparsePages[0]) / 8 ||
        !SafeRead(sparsePages[0] + (entityIndex / o::kSparsePage) * 8, sparsePage) || !LooksLikePointer(sparsePage) ||
        !SafeRead(sparsePage + (entityIndex % o::kSparsePage) * 4, position) || position == 0xFFFFFFFF) {
        return 0;
    }
    position &= o::kEntityMask;
    // El componente en su página (páginas de 128).
    std::uintptr_t payloadPages = 0;
    std::uintptr_t page = 0;
    if (!SafeRead(storage + o::kStoragePayload, payloadPages) || !LooksLikePointer(payloadPages) ||
        !SafeRead(payloadPages + (position / o::kPayloadPage) * 8, page) || !LooksLikePointer(page)) {
        return 0;
    }
    return page + (position % o::kPayloadPage) * size;
}

std::uintptr_t Component(std::uintptr_t actor, std::uint32_t typeHash, std::uintptr_t size) {
    std::uintptr_t registry = 0;
    std::uint32_t entityId = 0;
    if (!SafeRead(actor + o::kActorEnttRegistry, registry) || !LooksLikePointer(registry) ||
        !SafeRead(actor + o::kActorEntityId, entityId)) {
        return 0;
    }
    return ComponentOf(Storage(registry, typeHash), entityId, size);
}

}  // namespace luma::sdk::entity
