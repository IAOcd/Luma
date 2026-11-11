// =============================================================================
//  cloud.h - Talking to the Luma server (luma-api.gios-app.workers.dev)
// =============================================================================
//  So Luma users can recognize each other:
//    - "I'm using Luma": when loading and every 5 minutes (if you signed in
//      with Microsoft in the launcher: the launcher leaves your "Luma pass" in
//      %APPDATA%\Luma\session.dat, encrypted with DPAPI).
//    - "Which of these use Luma?": the player list asks about the names it
//      sees; the answer is kept for a few minutes.
//
//  Everything runs on its own thread: a slow connection never stalls the game.
//  Without internet, there are simply no logos.
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace luma::cloud {

void Start();
void Stop();

// Asks (without waiting) which of these names use Luma. Names that were
// asked recently aren't asked again.
void RequestLookup(const std::vector<std::string>& names);
// Does that player use Luma? (according to the server's last answer)
bool IsLumaUser(const std::string& name);
// Do you have a Luma pass (did you sign in with Microsoft in the launcher)?
bool SignedIn();

}  // namespace luma::cloud
