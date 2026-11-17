// =============================================================================
//  smtc_bridge.h - Telling Windows what Luma's own player plays
// =============================================================================
//  Spotify Soloist plays the sound but doesn't tell Windows WHAT plays. So the
//  launcher does: it shows the song in Windows' media controls (the panel of
//  the volume keys) with its name, artists, album, cover and position. Then
//  everything that reads Windows' music sees it: Luma's Home, the lyrics, the
//  game's HUD, and the keyboard's media keys (which come back here and
//  control Spotify).
// =============================================================================
#pragma once

#include <windows.h>

namespace luma::launcher::smtc {

void Init(HWND window);
// Each frame: is Luma's own player playing? (then the song shows in Windows;
// otherwise Luma's entry disappears from Windows' controls)
void Update(bool active);
void Shutdown();

}  // namespace luma::launcher::smtc
