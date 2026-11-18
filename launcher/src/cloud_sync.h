// =============================================================================
//  cloud_sync.h - "Save my styles and settings in my account" (OPTIONAL, off)
// =============================================================================
//  When you turn it on (Settings > Account) and you're signed in with
//  Microsoft, the launcher keeps a copy of these in your Luma account, on the
//  Luma server (luma-api, see account.h):
//    - your Lyric Flow styles      (%APPDATA%\Luma\config\flow_styles.json,
//                                   without the automatic "Your previous style")
//    - the Now Playing part of the active profile (its style, the visualizer,
//      Lyric Flow, Halo, where it sits...), WITHOUT your Spotify Client ID
//    - the launcher's theme, language and sky       (launcher.json)
//  On a new PC or after reinstalling: sign in, turn it on, and it all comes
//  back. Your styles from this PC are added, never lost.
//
//  HOW IT KEEPS TWO PCs IN STEP: the server keeps a version number ("rev")
//  that goes up by one with each save. This PC remembers the last one it saw
//  (%APPDATA%\Luma\sync.json). If the server has a newer one, another PC
//  saved: it's brought here. If you changed something here, it's sent 5 s
//  after your last change. If both changed (two PCs without internet), the
//  styles of both are kept and the settings of the cloud win.
//
//  Everything runs on its own thread: the window never waits for the internet.
//  Without internet it just waits and saves later.
// =============================================================================
#pragma once

#include <string>

namespace luma::launcher::cloudsync {

enum class Status {
    Off,           // the switch is off
    NeedsAccount,  // on, but you aren't signed in
    Working,       // talking to the server
    Synced,        // everything saved (lastSync says when)
    Offline,       // no connection: tries again later
    Error,         // errorKey says why
};

struct Info {
    bool enabled = false;
    Status status = Status::Off;
    long long lastSync = 0;       // seconds since 1970 (0 = never)
    std::string errorKey;         // translation key
    bool actionBusy = false;      // exporting / deleting
    std::string actionKey;        // the result of the last action (translation key), "" = none
    bool actionOk = true;
    std::string exportPath;       // where the data was saved (UTF-8)
};

void Start();
void Stop();
Info Get();

void SetEnabled(bool on);
// "Download my data": everything the server has about you, to a .json in Downloads.
void ExportData();
// "Delete from the cloud": only the synced copy (and the switch goes off).
void DeleteCloudCopy();
// "Delete my Luma account": EVERYTHING the server has about you, then signs out.
void DeleteAccount();

// The launcher's theme/language/sky were brought from the cloud: true once,
// then the window reads launcher.json again.
bool TakeLauncherReload();

}  // namespace luma::launcher::cloudsync
