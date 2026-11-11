// =============================================================================
//  dllmain.cpp - Luma.dll entry point
// =============================================================================
//  Windows calls DllMain when the DLL enters or leaves the game's process.
//  Here we do as LITTLE as possible: create Luma's own thread and nothing else.
//
//  Why? While DllMain runs, Windows holds an internal lock called the
//  "loader lock". If inside DllMain we did things like loading other DLLs or
//  waiting for other threads, we could end up waiting for something that is
//  itself waiting for that lock: the game would freeze forever (a
//  "deadlock"). That's why all the real work goes in MainThread.
// =============================================================================

#include <windows.h>

#include "core/luma.h"

namespace {

// The function Luma's thread runs. Windows only starts it once DllMain has
// finished, so here we can already do anything.
DWORD WINAPI MainThread(LPVOID parameter) {
    HMODULE module = static_cast<HMODULE>(parameter);

    // "try / catch (...)" catches any C++ error (exception) that slips out.
    // An uncaught exception inside the game would close it abruptly.
    try {
        luma::Run(module);
    } catch (...) {
        // There isn't much else to do: we move on to the unload step.
    }

    // Removes Luma.dll from the game's memory and ends this thread, in a single
    // step. It has to be one call: if we first freed the DLL and then tried to
    // end the thread, that second instruction would no longer exist in memory
    // (it was part of the DLL) and the game would crash.
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // We tell Windows not to notify us every time the game creates or ends
            // a thread: we don't care, and it saves work.
            DisableThreadLibraryCalls(module);

            HANDLE thread = CreateThread(nullptr, 0, MainThread, module, 0, nullptr);
            if (thread == nullptr) {
                // No thread, no Luma. Returning FALSE makes Windows discard
                // the DLL and the launcher reports that it failed.
                return FALSE;
            }
            // We don't need to wait for or control the thread from here, so
            // we release our handle (the thread keeps running anyway).
            CloseHandle(thread);
            break;
        }
        case DLL_PROCESS_DETACH:
            // We get here in two cases:
            //  a) Normal unload (reserved == nullptr): Shutdown() already cleaned
            //     everything up before FreeLibraryAndExitThread. Nothing to do.
            //  b) The game is closing (reserved != nullptr): Windows has already
            //     force-ended every thread, ours included, maybe in the middle of
            //     something. Touching the log or any lock could freeze the game's
            //     shutdown, so we do NOTHING.
            //     The log is already safe because every line is saved right away.
            (void)reserved;
            break;
        default:
            break;
    }
    return TRUE;
}
