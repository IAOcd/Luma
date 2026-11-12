// =============================================================================
//  waypoints.h - Waypoints: places you save (part C of the map)
// =============================================================================
//  A waypoint is a place with a name and a color ("Home", "Mine"...). The
//  minimap shows it as a dot, or as an arrow on its edge when it's far.
//
//  EACH WORLD HAS ITS OWN. Every 2 seconds Luma checks where you are:
//    - on a server: by its address ("play.server.net:19132");
//    - in a local world: by the world's name.
//  and keeps that world's waypoints in its own file:
//      %APPDATA%\Luma\waypoints\<world>.json
//  Each waypoint also remembers its dimension (Overworld, Nether, End), so
//  the Nether's don't show in the Overworld.
//
//  They only remember a place you chose: no advantage on a server, so they
//  work everywhere.
//
//  Used only from the render thread (the minimap and its settings), with
//  Luma's state lock held: no locks of its own.
// =============================================================================
#pragma once

#include <array>
#include <string>
#include <vector>

namespace luma::modules::world::waypoints {

struct Waypoint {
    std::array<char, 32> name{};  // (fixed size: the menu edits it in place)
    int x = 0, y = 0, z = 0;
    int dimension = 0;            // 0 Overworld, 1 Nether, 2 End
    float color[3] = {1.0f, 0.8f, 0.3f};
    bool visible = true;          // on the minimap
    bool inWorld = true;          // its beam and label in the world
    bool death = false;           // the automatic "where you died" (only the last one is kept)
};

// Once per frame (cheap: the real check is every 2 s). "dimension": where
// you are now (-1 = not known).
void Update(double now, int dimension);

// Is there a world (and so, a list)? And its name, for the menu.
bool HaveWorld();
const std::string& WorldLabel();
int Dimension();  // the one given to Update

// This world's waypoints (all dimensions).
std::vector<Waypoint>& List();

// A new one at (x, y, z) in the current dimension: name "Point N", the next
// color of a palette. Returns its index.
std::size_t Add(int x, int y, int z);
void Remove(std::size_t index);
// Where you died: replaces the previous death point of this world.
void SetDeathPoint(int x, int y, int z);

// Something changed: saved to disk a second later (and when leaving the world).
void MarkDirty();
// Saves now if something is pending (when Luma unloads or the module turns off).
void Flush();

}  // namespace luma::modules::world::waypoints
