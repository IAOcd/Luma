// =============================================================================
//  inventory.cpp - Implementación de la lectura del inventario
// =============================================================================
#include "sdk/inventory.h"

#include <cctype>
#include <cstdint>
#include <cstring>

#include "sdk/entity.h"
#include "sdk/memory.h"
#include "sdk/offsets.h"
#include "sdk/player.h"

namespace luma::sdk::inventory {

namespace {

namespace o = offsets;

// El "supplies" del jugador local, o 0.
std::uintptr_t Supplies() {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    std::uintptr_t supplies = 0;
    if (!sdk::player::Available() || player == 0 || !SafeRead(player + o::kPlayerSupplies, supplies) ||
        !LooksLikePointer(supplies)) {
        return 0;
    }
    return supplies;
}

// Dirección del ItemStack del lugar "slot", o 0.
std::uintptr_t StackAddress(int slot) {
    const std::uintptr_t supplies = Supplies();
    std::uintptr_t inventory = 0;
    std::uintptr_t vector[2]{};  // inicio y final del std::vector
    if (supplies == 0 || !SafeRead(supplies + o::kSuppliesInventory, inventory) || !LooksLikePointer(inventory) ||
        !SafeRead(inventory + o::kInventoryItems, vector) || !LooksLikePointer(vector[0])) {
        return 0;
    }
    const std::uintptr_t count = (vector[1] - vector[0]) / o::kItemStackSize;
    if (vector[1] < vector[0] || count > 64 || slot < 0 || static_cast<std::uintptr_t>(slot) >= count) {
        return 0;
    }
    return vector[0] + static_cast<std::uintptr_t>(slot) * o::kItemStackSize;
}

// Busca una clave numérica (IntTag) en el NBT del ítem. El NBT es un std::map ordenado por
// clave: es un árbol de búsqueda, así que bajamos comparando textos.
bool ReadIntTag(std::uintptr_t tag, const char* wanted, int& value) {
    std::uintptr_t head = 0;
    std::uintptr_t node = 0;
    if (!LooksLikePointer(tag) || !SafeRead(tag + o::kTagMap, head) || !LooksLikePointer(head) ||
        !SafeRead(head + o::kNodeParent, node)) {
        return false;
    }
    for (int depth = 0; depth < 32 && LooksLikePointer(node); ++depth) {
        std::uint8_t isNil = 1;
        if (!SafeRead(node + o::kNodeIsNil, isNil) || isNil != 0) {
            return false;
        }
        char key[32];
        if (!SafeReadString(node + o::kNodeKey, key, sizeof(key))) {
            return false;
        }
        const int cmp = std::strcmp(wanted, key);
        if (cmp == 0) {
            return SafeRead(node + o::kNodeIntValue, value);
        }
        if (!SafeRead(node + (cmp < 0 ? o::kNodeLeft : o::kNodeRight), node)) {
            return false;
        }
    }
    return false;
}

}  // namespace

int SelectedSlot() {
    const std::uintptr_t supplies = Supplies();
    int slot = -1;
    if (supplies == 0 || !SafeRead(supplies + o::kSuppliesSelectedSlot, slot) || slot < 0 || slot >= kHotbarSlots) {
        return -1;
    }
    return slot;
}

// Lee un ItemStack en la dirección "stack".
bool ReadStack(std::uintptr_t stack, Item& out) {
    out = Item{};
    std::uintptr_t weak = 0;
    std::uintptr_t item = 0;
    std::uint8_t count = 0;
    if (stack == 0 || !SafeRead(stack + o::kStackCount, count) || count == 0 || !SafeRead(stack + o::kStackItem, weak) ||
        !LooksLikePointer(weak) || !SafeRead(weak, item) || !LooksLikePointer(item) ||
        !SafeReadString(item + o::kItemId, out.id, sizeof(out.id)) || std::strchr(out.id, ':') == nullptr) {
        out = Item{};
        return false;
    }
    out.count = count;
    std::int16_t maxDamage = 0;
    SafeRead(item + o::kItemMaxDamage, maxDamage);
    out.maxDamage = maxDamage > 0 && maxDamage <= 10000 ? maxDamage : 0;
    std::uintptr_t tag = 0;
    SafeRead(stack + o::kStackTag, tag);
    if (out.maxDamage > 0) {
        int damage = 0;
        ReadIntTag(tag, "Damage", damage);
        out.damage = damage >= 0 && damage <= out.maxDamage ? damage : 0;
    }
    SafeReadString(item + o::kItemIcon, out.icon, sizeof(out.icon));
    // Color de la armadura de cuero teñida (sin el byte de transparencia).
    int color = 0;
    if (ReadIntTag(tag, "customColor", color)) {
        out.color = color & 0xFFFFFF;
    }
    return true;
}

bool ReadSlot(int slot, Item& out) {
    return ReadStack(StackAddress(slot), out);
}

namespace {

// El lugar "index" de uno de los contenedores del equipo (mano o armadura).
std::uintptr_t EquipmentStack(std::uintptr_t containerOffset, int index) {
    const std::uintptr_t player = sdk::player::LocalPlayer();
    if (!sdk::player::Available() || player == 0) {
        return 0;
    }
    const std::uintptr_t equipment = entity::Component(player, o::kEquipmentHash, o::kEquipmentSize);
    std::uintptr_t container = 0;
    std::uintptr_t vector[2]{};
    if (equipment == 0 || !SafeRead(equipment + containerOffset, container) || !LooksLikePointer(container) ||
        !SafeRead(container + o::kContainerItems, vector) || !LooksLikePointer(vector[0]) || vector[1] < vector[0]) {
        return 0;
    }
    const std::uintptr_t count = (vector[1] - vector[0]) / o::kItemStackSize;
    if (index < 0 || static_cast<std::uintptr_t>(index) >= count || count > 8) {
        return 0;
    }
    return vector[0] + static_cast<std::uintptr_t>(index) * o::kItemStackSize;
}

}  // namespace

bool ReadArmor(int piece, Item& out) {
    return ReadStack(EquipmentStack(o::kEquipmentArmor, piece), out);
}

bool ReadOffhand(Item& out) {
    return ReadStack(EquipmentStack(o::kEquipmentHand, 1), out);
}

int CountById(const char* id) {
    int total = 0;
    Item item;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (ReadSlot(slot, item) && std::strcmp(item.id, id) == 0) {
            total += item.count;
        }
    }
    return total;
}

void PrettyName(const char* id, char* out, int outSize) {
    const char* name = std::strchr(id, ':');
    name = name != nullptr ? name + 1 : id;
    int n = 0;
    bool upper = true;
    for (const char* c = name; *c != 0 && n < outSize - 1; ++c) {
        if (*c == '_') {
            out[n++] = ' ';
            upper = true;
        } else {
            out[n++] = upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(*c))) : *c;
            upper = false;
        }
    }
    out[n] = 0;
}

}  // namespace luma::sdk::inventory
