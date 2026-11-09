// =============================================================================
//  entity.cpp - Implementación de la lectura de componentes (ver entity.h)
// =============================================================================
#include "sdk/entity.h"

#include "sdk/memory.h"
#include "sdk/offsets.h"

namespace luma::sdk::entity {

namespace o = offsets;

std::uintptr_t Component(std::uintptr_t actor, std::uint32_t typeHash, std::uintptr_t size) {
    std::uintptr_t registry = 0;
    std::uint32_t entityId = 0;
    if (!SafeRead(actor + o::kActorEnttRegistry, registry) || !LooksLikePointer(registry) ||
        !SafeRead(actor + o::kActorEntityId, entityId)) {
        return 0;
    }

    // 1. Buscar la piscina del tipo en la tabla hash.
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
    std::uintptr_t storage = 0;
    for (int step = 0; step < 64 && index < 100000; ++step) {
        const std::uintptr_t node = packed + static_cast<std::uintptr_t>(index) * o::kPoolNodeSize;
        std::uint32_t key = 0;
        if (!SafeRead(node + o::kPoolNodeKey, key)) {
            return 0;
        }
        if (key == typeHash) {
            SafeRead(node + o::kPoolNodeStorage, storage);
            break;
        }
        if (!SafeRead(node + o::kPoolNodeNext, index)) {
            return 0;
        }
    }
    if (!LooksLikePointer(storage)) {
        return 0;
    }

    // 2. En qué posición está nuestra entidad.
    const std::uint32_t entityIndex = entityId & o::kEntityMask;
    std::uintptr_t sparsePages = 0;
    std::uintptr_t sparsePage = 0;
    std::uint32_t position = 0;
    if (!SafeRead(storage + o::kStorageSparse, sparsePages) || !LooksLikePointer(sparsePages) ||
        !SafeRead(sparsePages + (entityIndex / o::kSparsePage) * 8, sparsePage) || !LooksLikePointer(sparsePage) ||
        !SafeRead(sparsePage + (entityIndex % o::kSparsePage) * 4, position) || position == 0xFFFFFFFF) {
        return 0;
    }
    position &= o::kEntityMask;

    // 3. El componente en su página.
    std::uintptr_t payloadPages = 0;
    std::uintptr_t page = 0;
    if (!SafeRead(storage + o::kStoragePayload, payloadPages) || !LooksLikePointer(payloadPages) ||
        !SafeRead(payloadPages + (position / o::kPayloadPage) * 8, page) || !LooksLikePointer(page)) {
        return 0;
    }
    return page + (position % o::kPayloadPage) * size;
}

}  // namespace luma::sdk::entity
