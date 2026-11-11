// =============================================================================
//  clicks.h - Mouse click log (for CPS and keystrokes)
// =============================================================================
//  CPS = "clicks per second". We store the time of each click in a
//  "ring buffer" (a fixed array that keeps getting overwritten) and count how
//  many fall in the last second. No memory allocation and no locks (atomics).
//
//  Where do the clicks come from? From the same place the game reads them:
//  its mouse action queue (sdk/mouse_queue). If that signature isn't found,
//  from Windows messages (WM_LBUTTONDOWN...) as a fallback.
//
//  PERFORMANCE: clicks are only recorded if some active module uses them
//  (Acquire/Release). Otherwise the queue hook doesn't even look at clicks.
// =============================================================================
#pragma once

namespace luma::clicks {

enum class Button { Left = 0, Right = 1 };

// A module that needs the clicks calls Acquire when it turns on and Release
// when it turns off.
void Acquire();
void Release();
bool Wanted();

// A click (button pressed). From any thread.
void Record(Button button);

// Clicks in the last second.
int PerSecond(Button button);

}  // namespace luma::clicks
