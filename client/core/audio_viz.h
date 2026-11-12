// =============================================================================
//  audio_viz.h - What the music SOUNDS like right now (for the visualizer)
// =============================================================================
//  To make the sky dance with the music, Luma listens to the sound your PC is
//  playing and splits it into "bands" (low = bass, high = treble), like the
//  bars of a stereo's equalizer.
//
//  HOW IT LISTENS: Windows can give a program a copy of the sound that goes
//  to the speakers ("loopback"). Since Windows 10 (2021) it can even leave a
//  process out: Luma asks for EVERYTHING EXCEPT Minecraft, so the game's own
//  sounds (steps, mobs) don't move the bars, only your music. On older
//  Windows it falls back to all the sound.
//
//  Nothing is recorded or saved: the sound is analyzed in memory and
//  forgotten. It's the speakers' sound, not the microphone.
//
//  HOW IT ANALYZES: every ~16 ms it takes the last 2048 samples, applies a
//  "window" (so the edges don't create fake frequencies) and an FFT (Fast
//  Fourier Transform: a recipe that turns "the sound wave over time" into
//  "how much of each pitch it has"). Those pitches are grouped in bands on a
//  musical (logarithmic) scale and smoothed: bars go up fast and fall slowly.
//
//  COST: its own thread, only while the visualizer is on.
// =============================================================================
#pragma once

#include <cstdint>

namespace luma::audioviz {

inline constexpr int kBands = 64;
inline constexpr int kWavePoints = 192;  // the sound wave itself (for the layered wave style)

struct Levels {
    float bands[kBands]{};  // 0..1, low to high pitches (smoothed: they fall slowly)
    float fast[kBands]{};   // 0..1, the same but quick (they follow every hit)
    float peaks[kBands]{};  // 0..1, the highest recent value of each band (it waits, then falls)
    float wave[kWavePoints]{};  // -1..1, the last ~25 ms of the sound wave (auto-scaled)
    float kick = 0.0f;      // 0..1, a strong hit just happened (drum, bass drop): jumps, then fades fast
    float bass = 0.0f;      // 0..1, the low end (for pulses)
    float level = 0.0f;     // 0..1, how loud overall
    bool live = false;      // the capture is working
    bool processOnly = false;  // it's excluding Minecraft's own sounds
};

// Start / stop listening (the thread only runs while wanted).
void Want(bool want);
// Sensitivity (1 = normal) and smoothing (0 = jumpy, 1 = very smooth).
void SetTuning(float sensitivity, float smoothing);
// The latest levels (cheap copy).
Levels Get();

void Shutdown();

}  // namespace luma::audioviz
