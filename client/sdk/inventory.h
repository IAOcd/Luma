// =============================================================================
//  inventory.h - Leer el inventario del jugador local (Fase 7)
// =============================================================================
//  Camino (todo confirmado en 1.26.63 leyendo la memoria del juego):
//
//    jugador +0x5E0 -> "supplies" (PlayerInventory)
//        +0x10  ranura seleccionada de la barra rápida (0..8)
//        +0xB8  -> Inventory
//              +0x198  std::vector<ItemStack>: los 36 lugares
//
//    ItemStack (0x98 bytes): +0x08 -> -> Item, +0x10 -> NBT, +0x22 cantidad
//    Item: +0x128 id ("minecraft:mace"), +0x150 durabilidad máxima
//    NBT: el daño acumulado está en la clave "Damage" (si el ítem está gastado)
//
//  Todo con lecturas seguras (sdk/memory.h): si algo no cuadra, devuelve
//  false y el módulo no muestra nada.
// =============================================================================
#pragma once

namespace luma::sdk::inventory {

constexpr int kSlots = 36;
constexpr int kHotbarSlots = 9;

struct Item {
    char id[64] = "";      // "minecraft:diamond_sword"
    int count = 0;
    int damage = 0;        // usos gastados
    int maxDamage = 0;     // 0 = no se gasta
    char icon[48] = "";   // nombre de su ícono en el juego (vacío en bloques)
    int color = -1;        // color de teñido (cuero, NBT "customColor"), -1 = ninguno
};

// La ranura seleccionada (0..8), o -1 si no se pudo leer.
int SelectedSlot();

// Lee un lugar del inventario. false si está vacío o no se pudo leer.
bool ReadSlot(int slot, Item& out);

// La armadura puesta: 0 = casco, 1 = peto, 2 = pantalones, 3 = botas.
// (Vive en el componente de equipo del sistema de entidades: sdk/entity.h.)
bool ReadArmor(int piece, Item& out);
// El ítem de la mano secundaria (escudo, tótem...).
bool ReadOffhand(Item& out);

// Cuántos ítems con este id hay en todo el inventario.
int CountById(const char* id);

// Un nombre legible a partir del id: "minecraft:diamond_sword" -> "Diamond Sword".
void PrettyName(const char* id, char* out, int outSize);

}  // namespace luma::sdk::inventory
