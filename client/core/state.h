// =============================================================================
//  state.h - The lock for Luma's state
// =============================================================================
//  Several game threads touch Luma's data:
//    - the render thread (draws the menu and the HUD),
//    - the window thread (keys, mouse),
//    - Luma's main thread (saves the configuration).
//
//  So they never read something half written, EVERY access to modules,
//  configuration and ImGui is done with this lock held:
//
//      std::lock_guard lock(luma::StateMutex());
//      ...read or change modules/config...
//
//  It's "recursive" so the same thread can take it more than once (for
//  example, a function that takes it calls another one that also takes it).
//
//  Rule to avoid deadlocks: never hold it while calling an ORIGINAL
//  function of the game or of DirectX.
// =============================================================================
#pragma once

#include <mutex>

namespace luma {

inline std::recursive_mutex& StateMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

}  // namespace luma
