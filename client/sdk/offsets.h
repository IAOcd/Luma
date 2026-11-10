// =============================================================================
//  offsets.h - Dónde están los datos DENTRO de los objetos del juego
// =============================================================================
//  Un "offset" es cuántos bytes hay desde el inicio de un objeto hasta un
//  campo. Por ejemplo: el jugador está en la dirección X; en X + 0x240 hay
//  un puntero a su componente de posición.
//
//  Cambian con las actualizaciones del juego. Por eso:
//    1. Están TODOS aquí, en un solo lugar, con su versión.
//    2. Luma comprueba que tengan sentido antes de usarlos (ver player.cpp:
//       la caja del jugador tiene que medir ~0,6 x 1,8 bloques). Si no, los
//       módulos que los usan se desactivan y avisa: nunca lee basura.
//
//  CÓMO LOS ENCONTRAMOS (1.26.63): con el puntero del jugador que nos da el
//  hook de applyTurnDelta, leímos su memoria desde fuera del juego y
//  buscamos punteros a datos con forma conocida (3 floats de posición
//  repetidos, la gravedad -0,078 en la velocidad...). Coinciden con los que
//  usa el client open source Latite para 1.26.6x.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::offsets {

// --- Actor (cualquier entidad; el jugador local también lo es) -----------------
inline constexpr std::uintptr_t kActorTicksExisted = 0x1C0;    // int32: ticks de vida (20 por segundo)
inline constexpr std::uintptr_t kActorStateVector = 0x240;     // -> StateVectorComponent
inline constexpr std::uintptr_t kActorAabb = 0x248;            // -> AABBShapeComponent
inline constexpr std::uintptr_t kActorRotation = 0x250;        // -> ActorRotationComponent

// --- StateVectorComponent -----------------------------------------------------------
// Vec3 pos (a la altura de los OJOS: pies + 1,62), Vec3 posOld, Vec3 velocity
inline constexpr std::uintptr_t kStatePos = 0x0;
inline constexpr std::uintptr_t kStateVelocity = 0x18;

// --- AABBShapeComponent (la caja del jugador) --------------------------------------
// Vec3 min (los pies), Vec3 max, Vec2 tamaño (ancho, alto)
inline constexpr std::uintptr_t kAabbMin = 0x0;
inline constexpr std::uintptr_t kAabbSize = 0x18;

// --- ActorRotationComponent ------------------------------------------------------------
// float pitch (mirar arriba/abajo), float yaw (girar), y sus valores anteriores
inline constexpr std::uintptr_t kRotPitch = 0x0;
inline constexpr std::uintptr_t kRotYaw = 0x4;

// --- Player ------------------------------------------------------------------------
inline constexpr std::uintptr_t kPlayerSupplies = 0x5E0;   // -> PlayerInventory
inline constexpr std::uintptr_t kPlayerName = 0xBE8;       // std::string

// --- PlayerInventory ("supplies") ----------------------------------------------------
inline constexpr std::uintptr_t kSuppliesSelectedSlot = 0x10;  // int32 (0..8: la ranura de la barra rápida)
inline constexpr std::uintptr_t kSuppliesInventory = 0xB8;     // -> Inventory

// --- Inventory ------------------------------------------------------------------------
// std::vector<ItemStack> con los 36 lugares (0..8 = barra rápida).
inline constexpr std::uintptr_t kInventoryItems = 0x198;

// --- ItemStack (0x98 bytes cada uno) ---------------------------------------------------
inline constexpr std::uintptr_t kItemStackSize = 0x98;
inline constexpr std::uintptr_t kStackItem = 0x08;   // -> (-> Item)  ("WeakPtr": puntero a puntero)
inline constexpr std::uintptr_t kStackTag = 0x10;    // -> CompoundTag (NBT: encantamientos, daño...)
inline constexpr std::uintptr_t kStackCount = 0x22;  // uint8: cantidad

// --- Item (la "clase" del ítem: espada de diamante, flecha...) -------------------------
inline constexpr std::uintptr_t kItemIcon = 0x68;        // std::string: nombre de su ícono ("apple_golden"); vacío en bloques
inline constexpr std::uintptr_t kItemId = 0x128;         // std::string "minecraft:mace"
inline constexpr std::uintptr_t kItemMaxDamage = 0x150;  // int16: durabilidad máxima (0 = no se gasta)

// --- CompoundTag (NBT) -----------------------------------------------------------------
// Un std::map<std::string, Tag>. Cada nodo: izquierda +0, padre +8, derecha
// +0x10, "es el nodo vacío" +0x19, clave +0x20, valor +0x40.
inline constexpr std::uintptr_t kTagMap = 0x08;
inline constexpr std::uintptr_t kNodeLeft = 0x00;
inline constexpr std::uintptr_t kNodeParent = 0x08;
inline constexpr std::uintptr_t kNodeRight = 0x10;
inline constexpr std::uintptr_t kNodeIsNil = 0x19;
inline constexpr std::uintptr_t kNodeKey = 0x20;
inline constexpr std::uintptr_t kNodeIntValue = 0x48;  // el número de un IntTag (vtable en +0x40)

// --- Sistema de entidades (EnTT) ---------------------------------------------------------
// El juego guarda muchos datos de cada entidad como "componentes" en una
// base de datos (la biblioteca EnTT). Para leer uno: buscar su "piscina"
// (storage) por el hash de su tipo, y dentro, el lugar de nuestra entidad.
inline constexpr std::uintptr_t kActorEnttRegistry = 0x10;  // -> basic_registry
inline constexpr std::uintptr_t kActorEntityId = 0x18;      // uint32
inline constexpr std::uintptr_t kRegistryPoolsSparse = 0x48;  // std::vector<size_t> (cubetas)
inline constexpr std::uintptr_t kRegistryPoolsPacked = 0x68;  // std::vector<nodo> (32 bytes cada uno)
inline constexpr std::uintptr_t kPoolNodeNext = 0x00;
inline constexpr std::uintptr_t kPoolNodeKey = 0x08;
inline constexpr std::uintptr_t kPoolNodeStorage = 0x10;
inline constexpr std::uintptr_t kPoolNodeSize = 0x20;
inline constexpr std::uintptr_t kStorageSparse = 0x08;   // std::vector<páginas de uint32>
inline constexpr std::uintptr_t kStoragePayload = 0x50;  // std::vector<páginas de componentes>
inline constexpr std::uint32_t kEntityMask = 0x3FFFF;
inline constexpr std::uint32_t kSparsePage = 2048;
inline constexpr std::uint32_t kPayloadPage = 128;

// Componente de equipo: {Inventory* mano, Inventory* armadura}
inline constexpr std::uint32_t kEquipmentHash = 0xB06141A9;
inline constexpr std::uintptr_t kEquipmentSize = 0x10;
inline constexpr std::uintptr_t kEquipmentHand = 0x00;   // [0] mano principal, [1] mano secundaria
inline constexpr std::uintptr_t kEquipmentArmor = 0x08;  // [0] casco, [1] peto, [2] pantalones, [3] botas
inline constexpr std::uintptr_t kContainerItems = 0x1A0;  // std::vector<ItemStack> de estos contenedores

}  // namespace luma::sdk::offsets
