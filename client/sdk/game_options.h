// =============================================================================
//  game_options.h - Reading the game's own settings (like "Hide HUD")
// =============================================================================
//  The game keeps its settings (the ones in options.txt) in an "Options"
//  object. Each one has a NUMBER (an id; it changes between versions) and a
//  NAME that never changes: the one in options.txt, like "gfx_hidehud".
//
//  The game reads them through many tiny functions that are all the same
//  except for the number (getGamma = 0x32, getPerspective = 3, getHideHand =
//  0x1A6...): "ask the Options for setting N, then read its value". We find
//  them by signature to learn HOW a yes/no setting is read.
//
//  "Hide HUD" (the one F1 turns on and off) has no getter of its own, so
//  its NUMBER is read from the game's code where it creates the setting
//  named "gfx_hidehud" (see game_options.cpp). The number is never written
//  in Luma: it's found by the name.
//
//  If anything doesn't match, HideHud() says "unknown" (-1) and Luma counts
//  F1 presses instead (render/hud.cpp).
// =============================================================================
#pragma once

namespace luma::sdk::gameoptions {

// Finds the family of getters (once, when Luma loads).
bool Init();

// From a hook of one of those getters (sdk/camera.cpp), on the game's
// thread: the first time (and if the Options object changes) it asks the
// game for the "Hide HUD" setting.
void OnOptions(void* options);

// The game's "Hide HUD": 1 = on, 0 = off, -1 = unknown. Cheap (a few reads).
int HideHud();

}  // namespace luma::sdk::gameoptions
