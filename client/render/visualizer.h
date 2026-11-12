// =============================================================================
//  visualizer.h - The music visualizer in the sky, all around you
// =============================================================================
//  A ring of light around the horizon (or up in the sky) that dances with the
//  music: the bass on one side, the treble on the other, mirrored so the ring
//  closes smoothly. It's part of the sky like the lyrics' sky mode: very far,
//  around you, it follows you when you walk and stays put when you turn. The
//  terrain and the clouds in front cover it.
//
//  Styles: bars, mirrored bars, a wave line, an aurora (curtains of light up
//  high), rings above you, and thick pillars. Colors: one, the album's, the
//  album's gradient, two colors, or a rainbow around the ring.
//
//  The sound comes from core/audio_viz.h (everything except Minecraft); the
//  shapes are drawn by the game through sdk/draw3d.h, as lines.
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::visualizer {

// Every frame while it's on.
void Draw(ImVec2 display);
// Off: remove it from the sky and stop listening.
void Stop();

}  // namespace luma::render::visualizer
