// =============================================================================
//  music.h - How the music looks: the radial "Halo" and shared drawing
// =============================================================================
//  core/media.h knows WHAT is playing; this draws it. The "Now playing" module
//  (modules/hud/music.cpp) owns these settings and saves them; the radial
//  menu (render/menu.cpp) calls DrawHalo, and the HUD element and the Music
//  page use the helpers.
//
//  Render thread only (with luma::StateMutex held, like all of ImGui).
// =============================================================================
#pragma once

#include <imgui.h>

#include <cstddef>

#include "core/lyrics.h"
#include "core/media.h"

namespace luma::render::music {

// The control icons (24x24 SVG paths, "F" = filled), shared with the Music page.
inline constexpr const char* kIconPlay = "FM8 5.5V18.5L18.5 12Z";
inline constexpr const char* kIconPause = "FM7 5H10.5V19H7Z M13.5 5H17V19H13.5Z";
inline constexpr const char* kIconNext = "FM6 6V18L14.5 12Z M16 6H18.2V18H16Z";
inline constexpr const char* kIconPrev = "FM18 6V18L9.5 12Z M5.8 6H8V18H5.8Z";
inline constexpr const char* kIconShuffle =
    "M3 7H7C10.5 7 11.5 17 15 17H20 M3 17H7C8.6 17 9.6 15 10.5 13 M15 7H20 M17.5 4.5L20 7L17.5 9.5 M17.5 14.5L20 17L17.5 19.5";
inline constexpr const char* kIconRepeat =
    "M4 11V9.5C4 8.1 5.1 7 6.5 7H19 M16.5 4.5L19 7L16.5 9.5 M20 13V14.5C20 15.9 18.9 17 17.5 17H5 M7.5 14.5L5 17L7.5 19.5";
inline constexpr const char* kIconVolume =
    "M4 9.5H7.5L12 6V18L7.5 14.5H4Z M15.5 9.5C16.6 10.8 16.6 13.2 15.5 14.5 M18 7C20.4 9.6 20.4 14.4 18 17";

// Every look the user can change. Defaults = the "Halo" design.
struct Settings {
    bool active = false;       // the "Now playing" module is on (set by the module)
    // --- On screen (HUD element) ---
    int hudStyle = 1;          // 0 compact, 1 progress, 2 card, 3 one line, 4 vinyl, 5 lyrics, 6 wide + lyrics
    int artShape = 1;          // 0 square, 1 rounded, 2 round
    bool spinArt = true;       // round art turns while playing, like a record
    bool showArt = true;
    bool showArtist = true;
    bool showAlbum = false;
    bool showApp = false;      // "Spotify", "Chrome"...
    bool showTime = true;      // 1:37 / 4:03
    int accent = 1;            // 0 like the HUD text, 1 album color, 2 rainbow
    bool marquee = true;       // long names scroll instead of being cut
    int maxWidth = 150;        // in HUD pixels
    bool hideWhenPaused = false;
    bool hideWhenNothing = true;
    bool animateChanges = true;  // fade in the new song
    // --- Halo (around the radial menu) ---
    bool halo = true;
    float haloThickness = 0.32f;  // in font sizes
    float haloGap = 1.3f;         // distance from the radial, in font sizes
    int haloColor = 1;            // 0 Luma's accent, 1 album color, 2 rainbow
    bool haloTrack = true;        // the dim full ring behind the progress
    bool haloKnob = true;         // the dot at the current position (drag it to seek)
    bool haloArtCenter = true;    // the album art replaces your head in the middle
    int haloArtShape = 2;         // 0 square, 1 rounded, 2 round
    bool haloTitle = true;        // title and artist above the ring
    int haloButtons = 0;          // 0 under the ring, 1 around it, 2 none
    // --- Lyrics ---
    bool lyricsOnline = true;     // look songs up online (core/lyrics.h)
    float lyricsOffset = 0.0f;    // seconds: + = lyrics later, - = earlier
    bool lyricsNextLine = true;   // HUD lyrics: show the next line too
    bool hdCovers = true;         // look up a sharp album art online when the app's is small
    // --- Album colors everywhere ---
    bool hudFollowAlbum = false;  // every HUD module's text takes the album color
    bool menuFollowAlbum = false; // Luma's menu accent takes the album color
    bool crosshairAlbum = false;  // the Crosshair module takes the album color
    bool watermarkAlbum = false;  // the Watermark module takes the album color
};

Settings& Get();

// The album art as an ImGui texture (0 = none). Updates it when the song
// changes; one texture slot is reused forever.
ImTextureID ArtTexture();

// "97.4" -> "1:37".
void FormatTime(double seconds, char* out, std::size_t size);

// The color for "mode" (0 Luma accent / HUD text, 1 album, 2 rainbow).
ImVec4 AccentFor(int mode, const media::Track& track, ImVec4 fallback);

// The album art in a rectangle: shape 0 square, 1 rounded, 2 round (turned
// by "angle", radians). Without art: a dark square with a note.
void DrawArt(ImDrawList* draw, ImVec2 min, ImVec2 max, int shape, float angle, float alpha);

// The radial's Halo. "outer"/"inner" = the radial's radii. Returns true if
// the mouse is on the Halo (so the radial ignores that click) and sets
// "drewCenter" if the album art took the middle (so the radial skips your head).
bool DrawHalo(ImDrawList* draw, ImVec2 center, float outer, float inner, float appear, bool& drewCenter);

// How much the art has turned (shared, so every round art turns together).
float SpinAngle();

// The album color for the whole HUD / the menu, if those options are on and
// a song with art is playing. Computed once per frame (cheap to call a lot).
bool HudTint(float out[3]);
bool MenuTint(float out[3]);
// The album color for any module that offers "Album colors" (crosshair,
// watermark...): true while the Now playing module is on and the song has art.
bool AlbumColor(float out[3]);

// The progress shown on the Halo and the Music page. A seek doesn't jump:
// the dot travels there on a spring (it goes a little past and corrects).
// The spring runs on the trip's PROGRESS (0 -> 1), and how far it may go past
// the target is capped (2% of the bar): short seeks feel the same, long ones
// no longer swing wildly. After it, the new place is kept until the app
// reports it (apps take a moment, and the bar used to jump back and forth).
struct ProgressSpring {
    float x = -1.0f;          // shown position (0..1)
    float v = 0.0f;           // the trip's spring speed
    float hold = -1.0f;       // where we seeked to, while the app catches up
    double holdUntil = 0.0;
    std::uint32_t track = 0;
    float pop = 0.0f;         // 1 right after a seek, fades out (the dot "beats")
    float from = 0.0f;        // the seek animation: from where...
    double startedAt = -1.0;  // ...and when it started (-1 = not animating)
    float trip = 1.0f;        // the trip's progress (the spring moves this, 0 -> 1)
    // "playing" = the real position (0..1); "follow" = a target that wins
    // (while dragging), or a negative number for none.
    float Update(float playing, std::uint32_t trackId, double duration, float follow = -1.0f);
    void SeekTo(float frac, double duration);
};

// The lyrics of the current song (updated from core/lyrics when they change).
const lyrics::Result& CurrentLyrics();

// The song position for the lyrics, in ms (with the user's offset).
int LyricsMs(const media::Track& track);

// The lyrics panel of the Music page: synced lyrics scroll by themselves.
// Draws in the current ImGui window, inside min..max.
void DrawLyrics(ImVec2 min, ImVec2 max);

// The settings controls (used by the module's panel and by the Music page).
// They return true if something changed (so it gets saved).
bool DrawStyleSettings();
bool DrawHaloSettings();
bool DrawLyricsSettings();
bool DrawAlbumColorSettings();

}  // namespace luma::render::music
