// =============================================================================
//  radar.h - The entities around you, for the maps (part E) - LOCAL WORLDS ONLY
// =============================================================================
//  Luma's rule: nothing that gives an advantage in multiplayer. So this list
//  is only filled in YOUR OWN worlds (sdk/network says if you're on a
//  server: then it's empty, always).
//
//  WHERE IT COMES FROM: the game keeps the data of every entity in its
//  entity system (EnTT, see sdk/entity.h). Each one has a BOX (its size and
//  place, the same one the hitboxes module draws) and a TYPE number. The
//  type number carries "family" bits that the game itself uses:
//      0x100 mob, 0x800 monster, 0x1000 animal, 0x2000 water animal,
//      0x8000 ambient (bats), 0x1000000 villagers; 319 = player, 64 = item.
//  So Luma doesn't need a list of every mob: the family says what it is.
//
//  WHEN: read on the game's own drawing thread (like sdk/blocks), about 10
//  times a second, only while a map wants it. Never a crash: safe reads.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace luma::sdk::radar {

enum class Kind : std::uint8_t { Player, Hostile, Animal, Water, Ambient, Villager, Other, Item };

struct Entity {
    float x = 0, y = 0, z = 0;  // the middle of its feet
    std::uint32_t type = 0;     // the game's type number
    Kind kind = Kind::Other;
};

// Who wants the list (the work only happens while someone does).
void Want(bool want);

// Called by sdk/draw3d every frame, on the game's drawing thread.
void OnFrame();

// A copy of the latest list (empty on a server). Thread-safe.
void Copy(std::vector<Entity>& out);

// What a type number is (from its family bits).
Kind KindOf(std::uint32_t type);

}  // namespace luma::sdk::radar
