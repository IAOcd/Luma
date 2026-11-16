// =============================================================================
//  world_draw.h - Who draws Lyric Flow and the visualizer: Luma or the game
// =============================================================================
//  Lyric Flow and the visualizer build their shapes in WORLD coordinates (a
//  point is x, y, z in blocks, like the game's coordinates). Then there are
//  two ways to put them on the screen:
//
//  1. LUMA DRAWS THEM. Every frame, Luma takes the camera
//     (where it is, where it looks, how much it sees: sdk/camera.h) and works
//     out where each point of the world lands on the screen. That's a
//     "projection", the same math the game uses: a point straight ahead goes
//     to the middle, a point to the right goes right, and the farther away,
//     the closer to the middle (that's what makes far things look small).
//     Then it draws them with ImGui, on top of the game's picture, like the HUD.
//       + They look the same with any client (Flarial...) or texture pack:
//         they don't use any of the game's materials.
//       - Luma doesn't know what's in front of them in the world, so a
//         mountain or a wall never covers them.
//
//  2. THE GAME DRAWS THEM (the default; sdk/draw3d.h), as part of the world:
//     the blocks and entities in front cover them. They use the block outline's material, so a
//     client or a texture pack that changes the outline changes them too.
//
//  The choice is music::Settings::worldDraw (Music > Lyric Flow).
//
//  HILOS: todo desde el hilo de render, con luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <imgui.h>

#include <vector>

#include "sdk/draw3d.h"

namespace luma::render::worlddraw {

// true = Luma draws them (option 1), false = the game (option 2).
bool ByLuma();

// Can they be drawn right now? (Luma: the camera is known. The game: its
// drawing functions were found in this version.)
bool Ready();

// Replaces the shapes of one "channel" (sdk::draw3d::kChannelLyrics,
// kChannelVisualizer): call it every frame while there's something to show;
// an empty list = nothing. Besides the game's triangles and lines, Luma also
// understands "quads" (WorldShapes::quads: 4 points each).
void Submit(std::vector<sdk::draw3d::WorldShapes> shapes, int channel);

// Once per frame from the overlay, right after Lyric Flow and the visualizer
// were built: draws what was submitted (only in option 1).
void Draw(ImDrawList* draw, ImVec2 display);

// Luma's cost of the last Draw, in shapes (for the debug screen).
int LastPrimitiveCount();

}  // namespace luma::render::worlddraw
