// =============================================================================
//  logger.h - Luma's message log
// =============================================================================
//  The log is a text file where Luma writes down what it's doing:
//  "loaded", "hook installed", "signature not found", etc. It's our main
//  tool to understand what happened if something fails inside the game,
//  where we can't easily use a debugger.
//
//  Usage:
//      luma::log::Info("Luma {} loaded", luma::kVersion);
//      luma::log::Warn("Signature {} not found", name);
//      luma::log::Error("Failed to create the hook: {}", code);
//
//  The {} braces are replaced by the values that follow, in order
//  (std::format does this, part of C++20).
//
//  PERFORMANCE: every message builds a text and writes it to disk right away.
//  That costs on the order of microseconds: fine for events (load, unload,
//  errors), but it must NEVER be called every frame or every tick.
// =============================================================================
#pragma once

#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace luma::log {

// "enum class" = a closed list of named options.
enum class Level { Info, Warn, Error };

// Opens a new log file in "logDir" (creating the folder if needed) and
// deletes the oldest ones so there are never more than 10.
// If "withConsole" is true, it also opens a console window showing the
// log live.
// Returns false if it couldn't open the file (Luma keeps working the same,
// just without a log on disk).
bool Init(const std::filesystem::path& logDir, bool withConsole);

// Closes the file and the console. After this, messages are ignored.
void Shutdown();

// Writes an already built line. Normally the Info/Warn/Error below are used.
void Write(Level level, std::string_view message);

// Path of the current log file (empty if there's none).
const std::filesystem::path& CurrentFile();

// --- Formatting shortcuts -----------------------------------------------------
// "template" lets a single function accept arguments of any type and
// count. std::format_string also checks AT COMPILE TIME that the number of
// {} matches the number of arguments: a mistake there never reaches
// the game.
template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
    Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace luma::log
