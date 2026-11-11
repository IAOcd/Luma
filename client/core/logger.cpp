// =============================================================================
//  logger.cpp - Log implementation
// =============================================================================
#include "core/logger.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <share.h>  // _SH_DENYNO: let other programs read the file
#include <string>
#include <system_error>
#include <vector>

namespace luma::log {

// An unnamed "namespace" makes everything inside private to this file:
// nobody else can touch these variables.
namespace {

// How many log files are kept (the current one included).
constexpr std::size_t kMaxLogFiles = 10;
constexpr std::wstring_view kLogPrefix = L"luma_";
constexpr std::wstring_view kLogExtension = L".log";

// A "mutex" is a lock: if two threads want to write to the log at the same
// time, one waits for the other to finish. Without it, lines could get
// mixed together.
std::mutex g_mutex;
std::FILE* g_file = nullptr;
std::filesystem::path g_filePath;
HANDLE g_consoleOut = nullptr;  // console output, if it's open

// --- Console -----------------------------------------------------------------

// Windows notifies console programs when the user presses Ctrl+C.
// By default, that CLOSES THE PROCESS... which in our case is the game.
// This "handler" answers Windows "I handled it" (TRUE) and does nothing.
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ? TRUE : FALSE;
}

void OpenConsole() {
    if (!AllocConsole()) {
        return;  // There was already a console or Windows refused: we carry on without it.
    }
    SetConsoleTitleW(L"Luma - debug console");
    SetConsoleOutputCP(CP_UTF8);  // So accented characters show correctly.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Closing the console window with the X would also close the whole game.
    // To prevent that, we remove that window's close button.
    if (HWND window = GetConsoleWindow()) {
        if (HMENU menu = GetSystemMenu(window, FALSE)) {
            DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
        }
    }

    g_consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
}

void CloseConsole() {
    if (g_consoleOut == nullptr) {
        return;
    }
    // VERY IMPORTANT: remove the handler before unloading the DLL. Otherwise
    // Windows would keep pointing to a function that no longer exists in memory
    // and the next Ctrl+C would crash the game.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    FreeConsole();
    g_consoleOut = nullptr;
}

// --- Files ---------------------------------------------------------------------

// Local time in the requested format. "forFileName" uses dashes instead of ":"
// because Windows doesn't allow ":" in file names.
std::string Timestamp(bool forFileName) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    if (forFileName) {
        return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay,
                           t.wHour, t.wMinute, t.wSecond);
    }
    return std::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond,
                       t.wMilliseconds);
}

// Deletes the oldest logs so that, once the new one is created, kMaxLogFiles
// remain. Since the name starts with the date in year-month-day format,
// sorting by name is the same as sorting by age.
void DeleteOldLogs(const std::filesystem::path& dir) {
    // We use the version of the filesystem functions that takes an
    // "error_code": if something fails, it tells us in that variable instead of
    // throwing an exception. Inside the game we'd rather never throw.
    std::error_code ec;
    std::vector<std::filesystem::path> logs;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.starts_with(kLogPrefix) && name.ends_with(kLogExtension)) {
            logs.push_back(entry.path());
        }
    }
    if (logs.size() < kMaxLogFiles) {
        return;
    }
    std::sort(logs.begin(), logs.end());
    // We keep kMaxLogFiles - 1 to make room for the new file.
    const std::size_t toDelete = logs.size() - (kMaxLogFiles - 1);
    for (std::size_t i = 0; i < toDelete; ++i) {
        std::filesystem::remove(logs[i], ec);
    }
}

const char* LevelName(Level level) {
    switch (level) {
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

}  // namespace

bool Init(const std::filesystem::path& logDir, bool withConsole) {
    // "lock_guard" takes the lock and releases it by itself when leaving the function.
    std::lock_guard lock(g_mutex);

    if (withConsole) {
        OpenConsole();
    }
    if (logDir.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);  // doesn't fail if it already exists
    DeleteOldLogs(logDir);

    const std::string stamp = Timestamp(true);
    g_filePath = logDir / (std::wstring(kLogPrefix) +
                           std::wstring(stamp.begin(), stamp.end()) +
                           std::wstring(kLogExtension));

    // _wfsopen opens the file while letting other programs (for example
    // Notepad) read it while the game is open.
    // "ab" = append at the end, in binary mode (we write the text as is).
    g_file = _wfsopen(g_filePath.c_str(), L"ab", _SH_DENYNO);
    if (g_file == nullptr) {
        g_filePath.clear();
        return false;
    }
    return true;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    CloseConsole();
}

void Write(Level level, std::string_view message) {
    // We build the whole line outside the lock, so the lock is held for as
    // short a time as possible.
    const std::string line =
        std::format("[{}] [{}] {}\n", Timestamp(false), LevelName(level), message);

    std::lock_guard lock(g_mutex);
    if (g_file != nullptr) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        // fflush forces writing to disk RIGHT NOW. If the game crashes an
        // instant later, the line is still saved.
        std::fflush(g_file);
    }
    if (g_consoleOut != nullptr) {
        DWORD written = 0;
        WriteConsoleA(g_consoleOut, line.data(), static_cast<DWORD>(line.size()), &written,
                      nullptr);
    }
    // Also to the Windows "debugger" (visible with tools like DebugView).
    OutputDebugStringA(line.c_str());
}

const std::filesystem::path& CurrentFile() {
    return g_filePath;
}

}  // namespace luma::log
