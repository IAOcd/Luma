// =============================================================================
//  spotify.h - Optional Spotify connection (your queue: "Up next")
// =============================================================================
//  Windows tells Luma what's playing, but not what comes NEXT. Spotify's own
//  Web API does ("Get the user's queue"). With it Luma can:
//    - show your real queue in the Music page ("Up next");
//    - look up the lyrics of the next songs BEFORE they start, so when one
//      begins its word-by-word lyrics are already there.
//
//  HOW THE CONNECTION WORKS (all on your PC, no Luma server involved):
//    1. You create your own small "app" at developer.spotify.com (Spotify
//       requires it since 2026, and its owner needs Premium) and paste its
//       Client ID in Luma. The README explains it step by step.
//    2. "Connect" opens your browser on Spotify's official page. You sign in
//       THERE (Luma never sees your password) and accept that Luma can see
//       what you're playing and your queue. Nothing else.
//    3. Spotify sends you back to http://127.0.0.1:43821/callback: a tiny
//       server inside Luma that only exists for that moment, on your own PC.
//    4. Luma trades that code for a "pass" (token), using PKCE: a one-time
//       random secret proves it's the same Luma that asked, so no password
//       or app secret is stored anywhere.
//    5. The pass is saved ENCRYPTED with Windows' protection (DPAPI: only your
//       Windows user can read it) in %APPDATA%\Luma\spotify.dat.
//  You can disconnect in Luma (it deletes the file) or revoke the access in
//  your Spotify account at any time.
//
//  COST: its own thread, sleeping. It asks Spotify for the queue only when the
//  song changes (one small request), never in the render loop.
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace luma::spotify {

enum class State {
    NoClientId,  // no Client ID pasted yet
    SignedOut,
    SigningIn,   // waiting for you in the browser
    Connected,
    Error,
};

struct Status {
    State state = State::NoClientId;
    std::string error;  // a short reason when state == Error
    // Your account (once connected; empty / -1 until it's asked).
    std::string user;    // your name on Spotify
    int premium = -1;    // 1 Premium, 0 Free (Spotify only lets Premium control the music), -1 unknown
    // Connected with an older Luma that asked for fewer permissions (only to
    // see the queue): connect again to give the new ones (control the music,
    // your library, your playlists).
    bool outdated = false;
};

struct QueueItem {
    std::string title;
    std::string artist;  // "A, B" (like Windows shows it)
    std::string album;
    int seconds = 0;
};

// The Client ID of the user's Spotify app (from the music settings).
void SetClientId(const std::string& clientId);
// Turn on/off (off: the thread sleeps; the saved pass is kept).
void SetActive(bool active);
// Look up the lyrics of the next songs ahead (off: only the queue is shown).
void SetPrefetch(bool on);

// Opens the browser to connect. Does nothing if there's no Client ID.
void Connect();
// Forgets the pass (deletes spotify.dat).
void Disconnect();

Status GetStatus();

// The next songs (up to 10), if "version" changed. Empty when not connected.
bool Queue(std::uint32_t knownVersion, std::vector<QueueItem>& out, std::uint32_t& version);

void Shutdown();

// --- THE PLAYER (Spotify's Web API; controlling it needs Premium) ------------------------
// What plays in your Spotify, on any device (the PC, your phone...), and its
// controls. Asked about once a second while someone wants it.
struct Device {
    std::string id, name, type;  // type: "Computer", "Smartphone", "Speaker"...
    bool active = false;
    int volume = -1;             // 0..100 (-1 = unknown)
    bool canVolume = true;
};

struct Player {
    std::uint32_t version = 0;   // changes with every change
    bool valid = false;          // Spotify answered at least once
    bool hasDevice = false;      // something is open to play on (false: nothing playing anywhere)
    bool playing = false, shuffle = false;
    int repeat = 0;              // 0 off, 1 this song, 2 the list (album, playlist...)
    int progressMs = 0, durationMs = 0;
    std::chrono::steady_clock::time_point at;  // when progressMs was true (it moves on while playing)
    std::string trackId, title, artist, album;
    std::string trackUri, contextUri;  // the song and what it plays from (album, playlist...), to continue it elsewhere
    std::wstring artPath;        // its album art, downloaded (empty = none yet)
    bool liked = false, likedKnown = false;  // in your Liked songs
    Device device;               // where it plays
};

void SetPlayerWanted(bool wanted);  // (off: no questions to Spotify)
Player GetPlayer();
double PositionSeconds(const Player& p);  // where the song is now (seconds)
// Why the last command didn't work: "premium" (Spotify only lets Premium
// control the music), "no_device" (nothing open to play on), another short
// text, or empty.
std::string PlayerError();
void RequestDevices();  // asks for the list of your devices (it arrives in Devices)
bool Devices(std::uint32_t knownVersion, std::vector<Device>& out, std::uint32_t& version);

void PlayerPlay(bool play);
void PlayerNext();
void PlayerPrevious();
void PlayerSeek(double seconds);
void PlayerShuffle(bool on);
void PlayerRepeat(int mode);     // 0 off, 1 this song, 2 the list
void PlayerVolume(int percent);  // the device's volume (0..100)
void PlayerTransfer(const std::string& deviceId);  // play there
void PlayerLike(bool liked);     // add / remove the song from your Liked songs

// --- BROWSING (your library, search, pages) -----------------------------------------------
// The answer of a GET to the Web API ("/v1/me/playlists?limit=50"...), asked in
// the background and kept for a while (older than "maxAgeSeconds": asked
// again, the old one shown meanwhile). Returns 0 = asked (not here yet),
// 1 = here (in "out"), 2 = Spotify said no.
int Browse(const std::string& path, std::shared_ptr<const nlohmann::json>& out, int maxAgeSeconds = 300);
void Forget(const std::string& prefix);  // drop kept answers (after changing your library)
// A picture of Spotify's (a cover, an artist), downloaded once: its file, or
// empty while it comes.
std::wstring Image(const std::string& url);
// Any change: "PUT", "/v1/me/player/play", {"context_uri": ...} (play a
// playlist, add to the queue, to a playlist...). Errors show in PlayerError.
void Send(const char* method, const std::string& path, const std::string& body = {});
// LUMA'S RADIO: Spotify doesn't give its own radio to new apps (since 2024),
// so Luma makes one: the most played songs of these artists, mixed, after
// the seed song (if any), played now.
void PlayMix(const std::vector<std::string>& artistIds, const std::string& seedUri);

// The current access pass, for Luma's own player (the launcher's page with
// Spotify's Web Playback SDK, on this PC only). Empty the first moment: ask again.
std::string SharedToken();

// The address Spotify must send you back to (to register in your app).
inline constexpr const char* kRedirectUri = "http://127.0.0.1:43821/callback";

}  // namespace luma::spotify
