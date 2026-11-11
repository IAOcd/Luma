// =============================================================================
//  gpu_timer.h - How long the GPU is busy every frame
// =============================================================================
//  The frametime says how long a frame took, but not WHO made it slow:
//    - the CPU (the game simulating, building chunks, sending draw calls), or
//    - the GPU (drawing the pixels).
//  Render optimizations (Phase 9) only help when the GPU is the bottleneck,
//  so before optimizing we need to know which one it is.
//
//  How it works: every time the game sends work to its GPU queue
//  (ExecuteCommandLists), we put a tiny command list BEFORE and AFTER the
//  game's lists. Each one only writes a "timestamp" (the GPU's clock). The
//  difference is how long the GPU spent on that batch. Adding up all the
//  batches between two Present calls gives the GPU busy time of the frame.
//
//    GPU busy close to the frametime  -> the GPU is the limit.
//    GPU busy much lower              -> the CPU (or an FPS cap / vsync) is.
//
//  The results arrive a few frames late (the GPU works behind the CPU), so we
//  keep 4 "slots" in rotation and read a slot only when the GPU finished it.
//
//  COST: zero while nobody asks for it (one atomic read per submission).
//  While on, a few microseconds per frame on the CPU and two tiny command
//  lists per submission on the GPU. The command lists are recorded ONCE when
//  it's first turned on and reused forever: nothing is allocated per frame.
//
//  SAFETY: writing timestamps is the most harmless thing a command list can
//  do. If anything fails to be created, the timer simply stays off.
// =============================================================================
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace luma::perf::gputimer {

// Who wants the measurement. It runs while at least one of them does.
enum Source : std::uint32_t {
    kOverlay = 1u << 0,    // the performance overlay shows "GPU"
    kBenchmark = 1u << 1,  // a benchmark is recording
    kDebugScreen = 1u << 2,  // the debug screen (F3) is open
};
void Want(Source source, bool want);

// The game's direct queue and its device (from the overlay, once it has them).
void Attach(ID3D12Device* device, ID3D12CommandQueue* queue);

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// From the ExecuteCommandLists hook (any thread). Calls "original" with the
// game's lists, wrapped in our timestamps when measuring.
void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn original);

// From the Present hook (render thread), once per frame, before the real Present.
void OnPresent(ExecuteFn original);

// The GPU busy time of the last measured frame, in ms. Negative if there's no
// measurement (off, just turned on, or the frame had too many submissions).
float LastFrameMs();

// Releases everything (the overlay already waited for the GPU).
void Shutdown();

}  // namespace luma::perf::gputimer
