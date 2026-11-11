// =============================================================================
//  paths.h - Luma's data folders
// =============================================================================
//  Everything Luma saves to disk lives in %APPDATA%\Luma\ :
//    %APPDATA%\Luma\logs\        -> log files
//    (also: config, profiles, benchmarks, stats, skins)
//
//  %APPDATA% is a Windows folder for the user's program data,
//  usually C:\Users\<your user>\AppData\Roaming.
// =============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace luma::paths {

// Returns %APPDATA%\Luma. If Windows can't give us %APPDATA% (very rare),
// it returns an empty path and the caller decides what to do.
//
// "std::filesystem::path" is the C++ type for file paths; it takes care of
// the "\" slashes and special characters.
std::filesystem::path DataDir();

// Returns %APPDATA%\Luma\logs (or empty if DataDir() fails).
std::filesystem::path LogsDir();

// Converts a path to UTF-8 text to write it to the log.
// (C++'s path.string() can throw an error if the path has characters the
// Windows regional settings can't represent; this function never fails.)
std::string ToUtf8(const std::filesystem::path& path);

}  // namespace luma::paths
