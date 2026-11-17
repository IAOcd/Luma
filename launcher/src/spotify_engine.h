// =============================================================================
//  spotify_engine.h - LUMA'S OWN SPOTIFY PLAYER (music that plays from Luma)
// =============================================================================
//  Spotify lets other programs PLAY its music in only one official way: its
//  Web Playback SDK, a player that runs inside a web browser (it needs the
//  browser's content protection, "Widevine", and a Premium account).
//
//  The browser Windows programs can embed (WebView2) doesn't have that
//  protection, so Luma does this instead:
//    1. A tiny web server inside the launcher, ONLY on this PC
//       (http://127.0.0.1:43822), gives a minimal page with Spotify's player.
//    2. Microsoft Edge (it comes with Windows) opens that page, hidden and
//       with its own profile in %APPDATA%\Luma\edge (your own Edge isn't
//       touched), and allowed to play sound without a click.
//    3. The page asks the launcher for your access pass (only through
//       127.0.0.1, with a random key made each time) and connects: a device
//       called "Luma" appears in your Spotify, like your PC or your phone.
//    4. Playing there = the music comes out of Luma. Closing the launcher
//       closes that Edge too.
// =============================================================================
#pragma once

#include <string>

namespace luma::launcher::spotifyengine {

enum class State {
    Off,        // not started
    Starting,   // Edge opening, the player connecting
    Ready,      // the "Luma" device exists
    NoEdge,     // Microsoft Edge wasn't found
    Error,      // something failed (see Error())
};

void Start();  // (does nothing if it's already on)
void Stop();
State GetState();
std::string DeviceId();  // the "Luma" device's id, once Ready
std::string Error();

}  // namespace luma::launcher::spotifyengine
