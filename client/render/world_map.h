// =============================================================================
//  world_map.h - The world map, full screen (part D of the map)
// =============================================================================
//  Everything you explored (modules/world/map_store.h), on the whole screen:
//  drag to move, the wheel zooms where the cursor is, Space centers on you,
//  M or Esc closes it.
//
//  It works like the HUD editor: it's a "mode" of Luma's menu (the menu
//  already frees the mouse and keeps the game from seeing it), and while it's
//  active it's drawn instead of the menu.
//
//  HOW IT'S DRAWN: ONE texture the size of the screen. When the view changes
//  (you move it, zoom, or new blocks arrive) Luma fills it pixel by pixel
//  with the color of the block under each pixel. So every block is a crisp
//  square at any zoom (the GPU never stretches a small picture), and only
//  one of the overlay's texture places is used. Closed, it costs nothing.
// =============================================================================
#pragma once

namespace luma::render::worldmap {

bool IsActive();
// Starts it (centered on you). The caller opens Luma's menu.
void Open();
void Close();

// Inside menu::Draw, instead of the menu (with the state lock held).
void Draw();

}  // namespace luma::render::worldmap
