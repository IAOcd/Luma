// =============================================================================
//  config.h - Luma configuration and profiles (JSON)
// =============================================================================
//  Where it's stored:
//    %APPDATA%\Luma\config\luma.json            -> which profile is active
//    %APPDATA%\Luma\config\profiles\<name>.json -> everything else
//
//  A PROFILE is a complete configuration (active modules, their options and
//  keys, the theme, the menu and unload keys). You can have several
//  ("PvP", "Survival", ...) and switch between them from the menu.
//
//  Autosave: when something changes it's marked "waiting to be saved" and
//  Luma's main thread writes it one second later (so moving a slider doesn't
//  write the file 60 times per second). It first writes to a temporary file
//  and then replaces the real one: if the game closes right at that moment,
//  the file is never left half written.
//
//  If a file is damaged, a copy is kept (.corrupt.json), the defaults are
//  used and a toast warns about it. It never crashes.
//
//  THREADS: unless stated otherwise, call with luma::StateMutex() held.
// =============================================================================
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace luma::config {

// Global keys (inside the profile). Atomic because the window and the main
// thread read them without taking the lock.
extern std::atomic<int> g_menuKey;
extern std::atomic<int> g_unloadKey;

// Loads the active profile (at startup, from the main thread).
void LoadAtStartup();

// Marks that there are changes to save (any thread, no lock).
void MarkDirty();

// Saves if there have been pending changes for more than 1 s. Main thread,
// WITHOUT the lock held (it takes it only as long as needed).
void SaveIfDirty();

// Saves right now (when unloading Luma). Main thread, without the lock held.
void SaveNow();

// --- Profiles ------------------------------------------------------------------
const std::string& ActiveProfile();
std::vector<std::string> ListProfiles();

// Valid name: letters, numbers, spaces, '-' and '_', 1 to 32 characters.
bool IsValidProfileName(const std::string& name);

// Saves the current profile and loads another one.
bool SwitchProfile(const std::string& name);
// Creates a new profile by copying the current configuration and activates it.
bool CreateProfile(const std::string& name);
// Deletes a profile (not the active one).
bool DeleteProfile(const std::string& name);

// Resets everything in the active profile to the defaults.
void ResetActiveProfile();

}  // namespace luma::config
