// =============================================================================
//  gallery.h - The Lyric Flow style gallery (on the Luma server, see account.h)
// =============================================================================
//  ANYONE can look at the gallery and install a style (no account needed).
//  Publishing and reporting need the Microsoft sign-in, so nobody can pretend
//  to be someone else and whoever abuses it can be blocked.
//
//  A gallery style keeps only how the lyrics LOOK (colors, glow, effects,
//  font...), never where they are: installing one leaves your lyrics where
//  they were. Installing = it's added to "Your styles" and put on.
//
//  Your gamertag is shown next to a style you publish only if you tick it
//  (off by default); otherwise it says "Anonymous".
//
//  Everything talks to the server on one thread of its own: the window never
//  waits for the internet.
// =============================================================================
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace luma::launcher::gallery {

struct Style {
    std::string id;
    std::string name;
    std::string author;       // "" = anonymous
    nlohmann::json look = nlohmann::json::object();
    int installs = 0;
    long long created = 0;
    std::string status;       // (only yours) "visible" / "hidden"
};

enum class Sort { New = 0, Popular = 1 };

struct Info {
    // Explore
    Sort sort = Sort::New;
    bool loading = false;
    bool loaded = false;
    std::string errorKey;            // "" = fine
    std::vector<Style> styles;
    bool hasMore = false;
    // Yours (signed in)
    bool mineLoading = false;
    bool mineLoaded = false;
    bool banned = false;
    std::vector<Style> mine;
    // The style a luma:// link asked for (from the website), shown on top of Explore
    std::string linkedId;            // "" = none
    bool linkedLoading = false;
    std::string linkedErrorKey;
    Style linked;
    // The last thing you did (publish, remove, report...)
    bool busy = false;
    std::string actionKey;           // translation key, "" = none
    bool actionOk = true;
    int version = 0;                 // goes up with every change
};

// Explore: the first page (or the next one with "more").
void Load(Sort sort, bool more = false);
// Your published styles (and whether you can publish).
void LoadMine();
void Publish(const std::string& name, const nlohmann::json& look, bool showAuthor);
void Unpublish(const std::string& id);
// Counts the install (one per account). The style itself is added to your
// styles by the window (it's your file, no internet needed).
void CountInstall(const std::string& id);
// reason: "offensive_name", "spam" or "other".
void Report(const std::string& id, const std::string& reason);
// A luma://style/<id> link: fetch that one style (to show it on top).
void LoadLinked(const std::string& id);
void ClearLinked();
// Forget everything (signed out / another account).
void Reset();

Info Get();
void Stop();

// Only the "how it looks" part of a Lyric Flow style (what the gallery keeps).
nlohmann::json LookOnly(const nlohmann::json& look);

}  // namespace luma::launcher::gallery
