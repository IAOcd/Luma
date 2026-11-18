// =============================================================================
//  listening.h - "Your year in Luma": what you listened to while playing
// =============================================================================
//  WHAT IS RECORDED: while Minecraft is open and something is playing (any app
//  that tells Windows what it plays: Spotify, Luma's own player, the browser,
//  Apple Music...), the launcher adds up, for each song: its title, its
//  artist, how many seconds you heard it and how many times (a song counts as
//  "heard" after 30 seconds). Grouped by month. Nothing else: not when, not
//  which world or server, not which app.
//
//  WHERE: only on this PC (%APPDATA%\Luma\listening.json). It works without an
//  account. It can be turned off and deleted (Settings > Your year).
//
//  OPTIONAL, OFF BY DEFAULT: "Save my summary in my account" sends those same
//  monthly totals to the Luma server, so the summary joins all your PCs. Only
//  what changed is sent, every 10 minutes.
//
//  It runs on its own thread, also while the launcher is minimized (when it
//  doesn't draw anything): the window doesn't need to be on screen.
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace luma::launcher::listening {

struct Entry {
    std::string title;   // "" for an artist
    std::string artist;
    double seconds = 0.0;
    int plays = 0;
};

struct Summary {
    int year = 0;
    bool fromCloud = false;    // all your PCs (the server) or only this one
    double seconds = 0.0;      // music while playing, in total
    int plays = 0;
    int songs = 0;             // different songs
    int artists = 0;           // different artists
    std::vector<Entry> topSongs;    // up to 5
    std::vector<Entry> topArtists;  // up to 5
    double months[12] = {};    // seconds per month
    std::string shareUrl;      // (from your account) its public link, "" = not shared
    bool shareShowName = false;
};

struct Info {
    bool recording = true;     // recording on this PC (on unless you turn it off)
    bool upload = false;       // "save my summary in my account" (off)
    bool gameRunning = false;
    std::string nowTitle;      // what's being counted now ("" = nothing)
    std::string uploadKey;     // the upload's state (translation key), "" = fine / off
    long long lastUpload = 0;  // seconds since 1970
    bool cloudLoading = false;
    std::string actionKey;     // the result of the last action, "" = none
    bool actionOk = true;
    bool shareBusy = false;
    std::string linkUrl;       // the link just made (the window copies it once)
    int linkVersion = 0;       // goes up each time a link is made
    int version = 0;
};

void Start();
void Stop();
// From the launcher's status thread (every ~1.5 s): is Minecraft open?
void SetGameRunning(bool running);

Info Get();
void SetRecording(bool on);
void SetUpload(bool on);
// The summary of "year" from this PC's file (no internet).
Summary Local(int year);
// The summary from your account (all your PCs): asks the server (in the
// background) and returns the last answer; "ok" = there is one.
Summary Cloud(int year, bool& ok);
void RefreshCloud(int year);
// Delete what this PC recorded / what your account has.
void DeleteLocal();
void DeleteCloud();
// A public link to your year (from your account: "Save my summary in my
// account" must be on). "showName" = your gamertag on the page. The link
// stops working if you remove it, or delete what you listened to.
void Share(int year, bool showName);
void Unshare(int year);

}  // namespace luma::launcher::listening
