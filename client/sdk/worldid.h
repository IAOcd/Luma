// =============================================================================
//  worldid.h - Which world is this, exactly? (folder and seed)
// =============================================================================
//  Many worlds have the same name ("My World", "Meu Mundo"...), so the name
//  alone mixes their maps and waypoints. Two more facts, found by Luma by
//  itself the first time:
//
//  1. THE WORLD'S FOLDER (local worlds): each world lives in its own folder
//     of minecraftWorlds, with a unique name like "Rm8xZ2FyAAA=". The game
//     keeps that name in memory (in the Level object); Luma finds where by
//     looking for a text that is the name of one of your real folders.
//
//  2. THE SEED: written in that folder's level.dat ("RandomSeed"). Luma
//     reads it from the file, then looks for the same number in the world's
//     data in memory: that's where the game keeps it. From then on it's read
//     straight from memory, in any world (also on servers that send it).
//
//  The seed is shown in F3 only in your own worlds (on a server it would
//  help find structures and ores: an advantage). It's still used, inside,
//  to tell servers with the same world name apart.
//
//  Everything with safe reads; what can't be found is just "not known".
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace luma::sdk::worldid {

// Every couple of seconds (finds things the first time; cheap after).
void Update();

// The world's folder name ("" if unknown or not a local world).
const std::string& Folder();
// The seed (false if unknown).
bool Seed(std::int64_t& out);

}  // namespace luma::sdk::worldid
