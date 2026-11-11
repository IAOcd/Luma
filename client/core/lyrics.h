// =============================================================================
//  lyrics.h - The lyrics of the song that's playing (music module)
// =============================================================================
//  When the song changes (core/media.h), this looks for its lyrics in a list
//  of sources, in order, and keeps the best it finds:
//    1. word by word (each word with its own time: karaoke),
//    2. line by line (each line with its time: "synced"),
//    3. plain text (no times).
//
//  Sources today: LRCLIB (lrclib.net: free, no account, synced lyrics; some
//  songs also have word timings in "enhanced LRC"). The list is built so more
//  sources can be added (see lyrics.cpp, kSources).
//
//  PRIVACY: to look a song up, its title, artist, album and length are sent
//  to the source. That's all: nothing about you or your PC. It can be turned
//  off in the music settings ("Search lyrics online").
//  Found lyrics are kept in %APPDATA%\Luma\lyrics\, so a song is only looked
//  up once.
//
//  THREADS: its own thread; Snapshot() copies the result under a lock.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::lyrics {

struct Word {
    int ms = 0;          // when it starts
    std::string text;    // includes its trailing space, if any
};

struct Line {
    int ms = 0;               // when it starts (0 if not synced)
    std::string text;
    std::vector<Word> words;  // empty if the source has no word timings
};

enum class Status { Idle, Searching, Found, NotFound, Instrumental, Offline, Error };

struct Result {
    Status status = Status::Idle;
    std::uint32_t trackId = 0;   // the media trackId these lyrics belong to
    bool synced = false;         // lines have times
    bool wordSynced = false;     // words have times too
    std::string source;          // "LRCLIB"
    std::vector<Line> lines;
};

// Who needs lyrics (it works while at least one does).
enum Source : std::uint32_t {
    kMenu = 1u << 0,
    kHud = 1u << 1,
    kCovers = 1u << 2,  // only HD album art (no lyrics)
};
void Want(Source source, bool want);

// Look songs up online (off = only the ones already saved).
void SetOnline(bool online);

// HD COVERS: when the app gives Windows a small album art (Liner gives
// 150x150), this thread also looks the song up in Apple's iTunes catalog
// (itunes.apple.com: free, no account) and offers a 1000x1000 art to
// core/media. Only the title and artist are sent. Saved in
// %APPDATA%\Luma\covers\, so each song is looked up once.
void SetHdCovers(bool on);

void Shutdown();

// A copy of the current lyrics, if they changed since "knownVersion".
bool Snapshot(std::uint32_t knownVersion, Result& out, std::uint32_t& version);

// The line playing at "ms" (-1 = before the first one).
int LineAt(const Result& lyrics, int ms);

}  // namespace luma::lyrics
