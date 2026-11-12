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

namespace luma::render::pixel {
struct Form;
}

namespace luma::render::worldmap {

// What the world map shows (saved with the minimap's settings).
struct Settings {
    bool waypoints = true;    // the waypoint banners
    bool entities = true;     // the entity radar (local worlds only)
    bool names = true;        // their names
    int bannerSize = 0;       // 0 = like the minimap, 1-2 = bigger
    bool deathPoint = true;   // the "Death" waypoint
    bool chunkGrid = false;   // lines every 16 blocks (when zoomed in)
    bool regionGrid = false;  // lines every 512 blocks
    bool rulers = true;       // coordinates along the top and right edges
    bool coords = true;       // the block under the cursor
    bool follow = false;      // keep the map centered on you
    bool cost = false;        // how long it takes to paint (ms)
    float zoomSpeed = 1.0f;   // how much each wheel step zooms
    float startZoom = 2.0f;   // pixels per block when it opens
    bool remember = true;     // open where you left it (in the same world)
    bool sidebar = true;      // the sidebar open
    int tab = 0;              // which page of the sidebar
};
Settings& Config();
// Were the settings changed in the map since the last call? (to save them)
bool TakeSettingsChanged();

// The "Minimap" page of the sidebar: the minimap module draws its own
// settings there (nullptr = the module is off). Returns true if changed.
using MinimapPanel = bool (*)(render::pixel::Form& form);
void SetMinimapPanel(MinimapPanel panel);

bool IsActive();
// Esc: closes an open window of the map (true) or nothing (false: Esc
// then closes the whole map).
bool Escape();
// Starts it (centered on you). The caller opens Luma's menu.
void Open();
void Close();

// Inside menu::Draw, instead of the menu (with the state lock held).
void Draw();

}  // namespace luma::render::worldmap
