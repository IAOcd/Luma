// =============================================================================
//  dynlight.h - Dynamic lights: the game-memory part (Phase 9)
// =============================================================================
//  A light-giving item in your hand lights the world around you, only on
//  YOUR screen. Nothing in the world changes: the server, mob spawning and
//  crops keep seeing the real light.
//
//  How (Minecraft 1.26.63, all found by signature scanning):
//
//    1. BlockSource::getBrightnessPair(out, pos, minimumBlockLight) is the
//       game's "how much light is here?". It already takes a MINIMUM block
//       light. We raise that minimum near the light, but ONLY when the
//       question comes from:
//         - RenderChunkBuilder::build (building the mesh of a 16x16x16 piece
//           of the world: the light gets "baked" into it), or
//         - LevelRenderer::renderLevel (drawing the frame: entities, your
//           hand and your player get their brightness there).
//       A per-thread flag says "we're inside one of those".
//
//    2. Baked light only changes when the piece is rebuilt. To rebuild the
//       pieces around a moving light we call what the game calls when you
//       place a block: RenderChunkCoordinator::_setDirty(min, max,
//       immediate = true). ("Area changed" exists too, but it rebuilds
//       "later": the light took ~2 s to show.) There is one coordinator per
//       dimension, in a list at LevelRenderer+0x30; we use the one of the
//       dimension being drawn, on the thread that draws the world, like the
//       game does.
//
//  If a signature isn't found, dynamic lights are simply unavailable.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::sdk::dynlight {

bool Init();
bool Available();

// The light: block position and level (1..15). Level 0 = no light.
// From any thread (Luma's main thread). When it moves or changes, the
// pieces of the world around the old and the new light get rebuilt.
void SetLight(int x, int y, int z, int level);

// Light entities, your player and your hand too (not only blocks).
void SetLightEntities(bool on);

// Rebuilds at most this many times per second while the light moves.
void SetMaxRebuildsPerSecond(int perSecond);

// From the renderLevel hook (sdk/camera.cpp), on the thread that draws the
// world: before and after the game's renderLevel.
void EnterRenderLevel(std::uintptr_t levelRenderer);
void LeaveRenderLevel();

}  // namespace luma::sdk::dynlight
