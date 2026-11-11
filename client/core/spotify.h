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

#include <cstdint>
#include <string>
#include <vector>

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

// Opens the browser to connect. Does nothing if there's no Client ID.
void Connect();
// Forgets the pass (deletes spotify.dat).
void Disconnect();

Status GetStatus();

// The next songs (up to 10), if "version" changed. Empty when not connected.
bool Queue(std::uint32_t knownVersion, std::vector<QueueItem>& out, std::uint32_t& version);

void Shutdown();

// The address Spotify must send you back to (to register in your app).
inline constexpr const char* kRedirectUri = "http://127.0.0.1:43821/callback";

}  // namespace luma::spotify
