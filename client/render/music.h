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
#include <string>

#include "core/lyrics.h"
#include "core/media.h"
#include "render/icons.h"

namespace luma::render::music {

// The control icons (Lucide, see render/icons.h; "Filled" = solid), shared with the Music page.
inline constexpr const char* kIconPlay = icons::kPlayFilled;
inline constexpr const char* kIconPause = icons::kPauseFilled;
inline constexpr const char* kIconNext = icons::kSkipForwardFilled;
inline constexpr const char* kIconPrev = icons::kSkipBackFilled;
inline constexpr const char* kIconShuffle = icons::kShuffle;
inline constexpr const char* kIconRepeat = icons::kRepeat;
inline constexpr const char* kIconVolume = icons::kVolume;

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
    bool lyricsLiriqo = true;     // ask LiriQo first (word by word, third-party service)
    float lyricsOffset = 0.0f;    // seconds: + = lyrics later, - = earlier
    bool lyricsNextLine = true;   // HUD lyrics: show the next line too
    bool lyricsSweep = true;      // HUD lyrics: the light fills each word smoothly
    // --- Lyric Flow: the lyrics floating in the world (render/lyric_flow.h) ---
    bool lyricFlow = false;
    float flowDistance = 22.0f;   // blocks in front of you
    float flowHeight = 9.0f;      // blocks above your eyes
    float flowSize = 1.8f;        // a word's height, in blocks
    float flowTilt = 10.0f;       // how much the words lean (degrees, at most)
    float flowOpacity = 0.95f;
    bool flowUppercase = true;
    int flowMode = 0;             // 0 = anchored to a place of the world, 1 = in the sky (follows you, like the skybox)
    int flowLayout = 0;           // how a line is arranged: scattered, centered, left, arc, stacked, staircase
    int flowAppear = 0;           // how a word appears: pop, fade, drop, rise, zoom, spin, flip, typewriter, scatter
    int flowLeave = 0;            // how a line leaves: float up, fade, fall, shrink, scatter
    int flowMotion = 1;           // while it's there: none, float, wave, sway, pulse (the word being sung)
    float flowSpeed = 1.0f;       // the effects' speed (x)
    int flowColorMode = 0;        // 0 color, 1 album, 2 album gradient, 3 two colors, 4 rainbow
    float flowColor[3] = {1.0f, 1.0f, 1.0f};
    float flowColor2[3] = {0.70f, 0.56f, 1.0f};
    bool flowHighlight = false;   // the word being sung in its own color
    float flowHighlightColor[3] = {1.0f, 0.84f, 0.40f};
    bool flowOutline = true;      // a border around the letters
    float flowOutlineColor[3] = {0.05f, 0.05f, 0.07f};
    float flowOutlineAlpha = 0.85f;
    bool flowShadow = false;      // a dark copy a little behind and below
    int flowFont = 0;             // 0 = Minecraft's pixel font, others = Windows fonts (render/lyric_flow.h kFonts)
    bool flowBold = true;         // (Windows fonts) bold
    int flowResolution = 16;      // (Windows fonts) how many "pixels" tall a letter is: more = smoother, more lines
    float flowLetterSpacing = 0.0f;  // extra space between letters, in letter pixels (-1..4)
    float flowWordSpacing = 0.4f;    // space between words, in word heights
    float flowLeaveTime = 0.45f;     // how long a line takes to leave (seconds)
    bool flowAlbumColor = false;  // (old setting: now flowColorMode = 1)
    int flowMaterial = 0;         // (unused: the letters are lines with the outline material)
    // MUSIC EFFICIENCY MODE (like the general one, only for the music): it
    // overrides these without changing them. "eff*" = what it turns off.
    bool efficiency = false;
    bool effLyricAnim = true;    // lyrics: light sweep, rising words, fades, gliding list
    bool effUiAnim = true;       // the Music page's buttons and controls (press, ripple, pop)
    bool effSpin = true;         // the round art doesn't spin
    bool effSongFade = true;     // no fade when the song changes
    bool effMarquee = true;      // long names are cut instead of scrolling
    bool effAlbumColors = true;  // no album colors on the HUD, menu, crosshair, watermark
    bool effHdCovers = true;     // no HD album art downloads
    bool effPrefetch = true;     // no lyrics looked up ahead (Spotify queue)
    std::string spotifyClientId;  // the user's own Spotify app (core/spotify.h); empty = off
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

// How much of word "word" of line "line" is already sung, 0..1. Each word
// lasts until the next one starts; the last word of a line, until the next
// line (1.5 s at most). This makes the light FILL each word at the speed it's
// sung (a long held note fills slowly), instead of switching it on at once.
float WordProgress(const lyrics::Result& ly, int line, std::size_t word, int ms);

// Soft edge: the text drawn since vertex "firstVertex" fades out to the right
// of "edgeX", over "feather" pixels. Drawing a bright copy of a word on top
// of a dim one and moving the edge = a smooth sweep of light.
// (It changes the transparency of the letters' corners: no extra drawing.)
void FeatherRight(ImDrawList* draw, int firstVertex, float edgeX, float feather);

// 0..1 -> 0..1, slow at the start and the end (for animations).
inline float SmoothStep(float x) {
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x * x * (3.0f - 2.0f * x);
}

// The EFFECTIVE answers (the setting, unless the music efficiency mode turns
// it off). Cheap: a couple of reads.
bool LyricAnimations();
bool UiAnimations();
bool SpinOn();
bool SongFade();
bool MarqueeOn();
bool AlbumColorsOn();
bool HdCoversOn();
// Tells the background threads (HD covers, Spotify prefetch) what's allowed.
void ApplyEfficiency();
// The music efficiency section (Music > Styles).
bool DrawEfficiencySettings();

// "Up next" tab of the Music page: your Spotify queue (core/spotify.h).
void DrawUpNext(ImVec2 min, ImVec2 max);
// The Spotify section of the settings (Client ID, connect / disconnect).
bool DrawSpotifySettings();

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
