// =============================================================================
//  stats.h - Game stats and player name (for the launcher)
// =============================================================================
//  Luma writes two files in %APPDATA%\Luma\ that the launcher reads:
//
//    stats.json   -> {"sessions": 12, "playSeconds": 45210, "lastPlayed": 1794300000}
//                    A "session" = one time Luma was loaded into the game. Time
//                    only counts while Luma is loaded. It's saved every minute
//                    (if the game closes abruptly, at most one minute is lost)
//                    and when Luma unloads.
//    profile.json -> {"name": "YourGamertag", "gameDir": "C:/XboxGames/.../Content"}
//                    Your player's name (in Bedrock it's your Xbox gamertag),
//                    read from the game when you join a world, and the game
//                    folder (the launcher takes the pixel font from there).
//    skin.png + skin.json -> your skin, read from the game every 10 s (only
//                    written if it changed). See sdk/skin.h.
//
//  Everything runs on Luma's main thread (the one that waits for the unload
//  key), never on the render thread: writing to disk there would cost frames.
// =============================================================================
#pragma once

namespace luma::stats {

// When Luma loads: counts a new session.
void Begin();
// Every loop of the main thread (~50 ms): only does something every so often.
void Poll();
// When Luma unloads: saves the time still missing.
void End();

}  // namespace luma::stats
