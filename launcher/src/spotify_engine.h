// =============================================================================
//  spotify_engine.h - LUMA'S OWN SPOTIFY PLAYER (luma-spotify.exe)
// =============================================================================
//  A small program of Luma's (launcher/player, written in Rust on librespot,
//  the same engine as Spotifast's built-in player) that becomes a Spotify
//  Connect device called "Luma" and plays through Windows' sound, without the
//  Spotify app.
//    - The first time it opens Spotify's page in your browser: you approve
//      there, once. It keeps its pass in %APPDATA%\Luma\spotify-player.
//    - Luma runs it hidden (no window), inside Luma: it closes with Luma.
//  librespot is not made by Spotify (it speaks Spotify's protocol): it's what
//  Spotifast and other players use; Spotify's terms don't allow it.
// =============================================================================
#pragma once

#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace luma::launcher::spotifyengine {

enum class State {
    NoProgram,  // luma-spotify.exe isn't next to the launcher
    Off,        // not running
    Login,      // waiting for you to approve in the browser (first time)
    Starting,   // connecting
    Ready,      // the "Luma" device is in your Spotify
    Error,      // see Error()
};

constexpr const char* kDeviceName = "Luma";

void Start();      // run it (hidden); it becomes Ready when Spotify lists the device
void Stop();
void SignOut();    // forget its pass (the next start asks in the browser again)
State GetState();  // (call each frame: it also notices when the device appears)
// A direct command to the player (no trip through Spotify's servers):
//   "load <context-uri> <track-uri or -> <position-ms>", "tracks <index> <position-ms> <uri>...",
//   "play", "pause", "next", "prev"
void Command(const std::string& line);
void SetVolume(int percent);  // (0..100: Windows' volume of the player's sound, instant; remembered)
int Volume();
// The songs of a playlist through the player (Spotify's internal channel):
// for other people's playlists, which the public Web API no longer gives to
// apps. Starts the player if needed (it only connects). 0 = asked, 1 = here
// (in "out", like the Web API's playlist: {"items": {"items": [{"item": track}]}}), 2 = failed.
int PlaylistSongs(const std::string& playlistId, std::shared_ptr<const nlohmann::json>& out);
// Any question to the player: "playlist <id>", "radio <uri>" ({"uri": Spotify's
// radio playlist}), "artist_top <id>". "again": ask anew (a new radio...).
int Ask(const std::string& question, std::shared_ptr<const nlohmann::json>& out, bool again = false);
std::string DeviceId();
std::string Error();

}  // namespace luma::launcher::spotifyengine
