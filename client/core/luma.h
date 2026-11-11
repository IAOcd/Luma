// =============================================================================
//  luma.h - Luma's lifecycle (start, wait, unload)
// =============================================================================
#pragma once

#include <windows.h>

namespace luma {

// Runs all of Luma: starts the systems, waits until the user presses the
// unload key and then shuts everything down in order. It runs on Luma's own
// thread (see dllmain.cpp), never inside DllMain.
//
// When this function returns, NOTHING of Luma is active in the game anymore
// (no hooks, no threads, no windows), and it's safe to remove the DLL from memory.
void Run(HMODULE module);

}  // namespace luma
