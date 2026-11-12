// =============================================================================
//  media.h - What's playing on Windows, and its controls (music module)
// =============================================================================
//  Windows keeps a list of "media sessions": every app that plays music or
//  video and tells Windows about it (Spotify, the browser with YouTube or
//  SoundCloud, Apple Music, Windows Media Player...). It's the same list the
//  Windows 11 volume flyout shows. The official API is
//  GlobalSystemMediaTransportControlsSessionManager (Windows.Media.Control).
//
//  From it we read the current session: title, artist, album, album art,
//  playing or paused, position and length, shuffle and repeat. And we can
//  send it commands: play/pause, next, previous, seek, shuffle and repeat
//  (if the app allows each one).
//
//  VOLUME isn't part of that API. It's done like Windows' volume mixer: we
//  find the app's audio session (by its name) and change only its volume.
//  FAVORITES ("like") can't be done: Windows has no way to ask an app to
//  like a song.
//
//  THREADS: everything talking to Windows runs on media's own thread, never
//  on the game's render thread. Every Windows call has a time limit, so an
//  app that hangs can't freeze the game or the unload.
//  PERFORMANCE: the thread only works while someone wants it (Want); the
//  rest of the time it sleeps. Current() is a copy under a small lock.
//  It doesn't read any game memory: it's not in /sdk.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace luma::media {

constexpr int kArtSize = 512;  // the album art, always as a 512x512 RGBA picture (sharp on the Music page)

struct Track {
    bool present = false;      // is anything reporting media?
    bool playing = false;
    char title[160] = "";
    char artist[160] = "";
    char album[160] = "";
    char app[48] = "";         // "Spotify", "Chrome"...
    double position = 0.0;     // seconds (moves on its own while playing)
    double duration = 0.0;     // seconds (0 = unknown: some apps don't say)
    bool canPlayPause = false, canNext = false, canPrevious = false, canSeek = false;
    bool canShuffle = false, shuffle = false;
    bool canRepeat = false;
    int repeat = 0;                // 0 off, 1 this song, 2 the whole list
    float volume = -1.0f;          // the app's volume 0..1 (-1 = its audio wasn't found)
    std::uint32_t trackId = 0;     // changes when the song changes
    std::uint32_t artVersion = 0;  // changes when the album art changes (0 = no art)
    int artSourceSize = 0;         // width of the art the app gave Windows (0 = none). Small = blurry.
    bool artChecked = false;       // the app's art was already looked at for this song
    float accent[3] = {0.91f, 0.90f, 0.87f};  // main color of the album art (ivory if none)
    // The album's PALETTE: its most present colors (2..5), the most present first.
    float palette[5][3] = {{0.91f, 0.90f, 0.87f}, {0.70f, 0.68f, 0.64f}};
    int paletteCount = 2;
};

// Who needs the music (it runs while at least one does).
enum Source : std::uint32_t {
    kHud = 1u << 0,
    kRadial = 1u << 1,
    kMenu = 1u << 2,
};
void Want(Source source, bool want);

// Stops the thread and releases everything (on unload). Waits for it.
void Shutdown();

// A copy of what's playing now. Cheap (any thread).
Track Current();

// The album art, if it's newer than "knownVersion": kArtSize x kArtSize RGBA into "out".
bool TakeArt(std::uint32_t knownVersion, std::vector<std::uint8_t>& out, std::uint32_t& version);

// Commands (they run on media's thread a moment later).
void PlayPause();
void Next();
void Previous();
void Seek(double seconds);
void SetShuffle(bool on);
void SetRepeat(int mode);      // 0 off, 1 this song, 2 the whole list
void SetVolume(float volume);  // 0..1, only the app that's playing (Windows' volume mixer)

// A better album art for the song "trackId" (the bytes of a JPEG/PNG), found
// online by core/lyrics.cpp when the app's own art is small. Used if that
// song is still playing.
void OfferArt(std::uint32_t trackId, std::vector<std::uint8_t> bytes);

}  // namespace luma::media
