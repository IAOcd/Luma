// =============================================================================
//  hooks.h - Wrapper over MinHook
// =============================================================================
//  What's a hook? It "intercepts" a function: every time the game calls it,
//  OUR function (the "detour") runs first, and it can do something and then
//  call the original function (through the "trampoline" MinHook gives us).
//
//      game --calls--> Present  ==hook==>  our detour
//                                             | draws Luma
//                                             v
//                                           trampoline --> original Present
//
//  MinHook does this by rewriting the first bytes of the original function
//  with a jump to our detour. When the hook is disabled, it restores those
//  bytes and everything is back the way it was.
//
//  This file also keeps count of how many threads are INSIDE some detour
//  right now. When unloading Luma we have to wait for that count to reach 0:
//  if we removed the DLL from memory while a game thread was still running
//  our code, the game would crash.
// =============================================================================
#pragma once

#include <atomic>

namespace luma::hooks {

// Sets up MinHook. Call once at startup.
bool Init();

// Creates a hook (still disabled).
//   target   -> the function to intercept.
//   detour   -> our replacement function.
//   original -> MinHook writes the trampoline's address here, which we use
//               to call the original function.
//   name     -> name for the log.
bool Create(void* target, void* detour, void** original, const char* name);

// Enables / disables every hook created.
bool EnableAll();

// Enables a single hook (for hooks created later, with Luma already
// running).
bool Enable(void* target, const char* name);
void DisableAll();

// Waits until no thread is inside a detour. See ActiveCallGuard.
void WaitForActiveCalls();

// Releases MinHook (removes the hooks completely). Call after DisableAll and
// WaitForActiveCalls.
void Shutdown();

// --- Active call counter ------------------------------------------------------
// Every detour creates one of these when it starts. The constructor adds 1 to
// the counter and the destructor (which C++ calls automatically when leaving
// the function, whichever way it leaves) subtracts 1.
//
// "std::atomic" is a number several threads can modify at the same time
// without corrupting it.
extern std::atomic<int> g_activeCalls;

struct ActiveCallGuard {
    ActiveCallGuard() { g_activeCalls.fetch_add(1, std::memory_order_acq_rel); }
    ~ActiveCallGuard() { g_activeCalls.fetch_sub(1, std::memory_order_acq_rel); }
    ActiveCallGuard(const ActiveCallGuard&) = delete;
    ActiveCallGuard& operator=(const ActiveCallGuard&) = delete;
};

}  // namespace luma::hooks
