// =============================================================================
//  listening.cpp - What you listened to while playing (see listening.h)
// =============================================================================
#include "listening.h"

#include "account.h"
#include "net.h"
#include "core/media.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace luma::launcher::listening {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr double kHeardAfter = 30.0;                       // seconds of a song = one "play"
constexpr auto kTick = std::chrono::seconds(5);
constexpr auto kSaveEvery = std::chrono::seconds(60);
constexpr auto kUploadEvery = std::chrono::minutes(10);
constexpr auto kRetry = std::chrono::minutes(2);
constexpr std::size_t kMaxItems = 20000;                   // (a safety limit for the file)

struct Item {
    int y = 0, m = 0;
    std::string artist, title;
    double seconds = 0.0;
    int plays = 0;
    long long sentSeconds = 0;  // already in your account
    int sentPlays = 0;
};

std::mutex g_mutex;  // guards everything below
std::condition_variable g_wake;
std::thread g_thread;
bool g_quit = false;
std::atomic<bool> g_gameRunning{false};
std::vector<Item> g_items;
std::map<std::string, std::size_t> g_index;  // "y|m|artist|title" -> item
bool g_recording = true;
bool g_upload = false;
std::string g_account;   // whose account the "sent" counters are
bool g_dirty = false;
bool g_uploadNow = false;
Info g_info;
// The cloud's summary (the last answer)
Summary g_cloud;
bool g_cloudOk = false;
int g_cloudWanted = 0;

fs::path File() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma" / L"listening.json";
    }
    CoTaskMemFree(raw);
    return path;
}

std::string Key(int y, int m, const std::string& artist, const std::string& title) {
    return std::to_string(y) + "|" + std::to_string(m) + "|" + artist + "\x1F" + title;
}

void Reindex() {
    g_index.clear();
    for (std::size_t i = 0; i < g_items.size(); ++i) g_index[Key(g_items[i].y, g_items[i].m, g_items[i].artist, g_items[i].title)] = i;
}

// (all under g_mutex)
void Load() {
    std::ifstream in(File(), std::ios::binary);
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    g_recording = j.value("recording", true);
    g_upload = j.value("upload", false);
    g_account = j.value("account", std::string());
    if (j.contains("items") && j["items"].is_array()) {
        for (const json& e : j["items"]) {
            if (!e.is_object()) continue;
            Item it;
            it.y = e.value("y", 0);
            it.m = e.value("m", 0);
            it.artist = e.value("artist", std::string());
            it.title = e.value("title", std::string());
            it.seconds = e.value("seconds", 0.0);
            it.plays = e.value("plays", 0);
            it.sentSeconds = e.value("sentSeconds", 0LL);
            it.sentPlays = e.value("sentPlays", 0);
            if (it.y < 2000 || it.m < 1 || it.m > 12 || it.title.empty()) continue;
            g_items.push_back(std::move(it));
        }
    }
    Reindex();
}

void Save() {
    json items = json::array();
    for (const Item& it : g_items) {
        items.push_back({{"y", it.y}, {"m", it.m}, {"artist", it.artist}, {"title", it.title}, {"seconds", std::round(it.seconds)},
                         {"plays", it.plays}, {"sentSeconds", it.sentSeconds}, {"sentPlays", it.sentPlays}});
    }
    const json j = {{"version", 1}, {"recording", g_recording}, {"upload", g_upload}, {"account", g_account}, {"items", std::move(items)}};
    const fs::path file = File();
    if (file.empty()) return;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path temp = fs::path(file).concat(L".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump();
        if (!out) return;
    }
    MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    g_dirty = false;
}

std::string Trim(const char* text, std::size_t max) {
    std::string s = text;
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ') s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && static_cast<unsigned char>(s[start]) <= ' ') ++start;
    s = s.substr(start);
    if (s.size() > max) {
        s.resize(max);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // (not half a letter)
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
    }
    return s;
}

// --- The summary ------------------------------------------------------------------------
Summary Summarize(int year) {
    Summary s;
    s.year = year;
    std::map<std::string, Entry> artists;
    std::vector<Entry> songs;
    std::map<std::string, std::size_t> songIndex;  // the same song in different months = one
    for (const Item& it : g_items) {
        if (it.y != year) continue;
        s.seconds += it.seconds;
        s.plays += it.plays;
        s.months[it.m - 1] += it.seconds;
        const std::string key = it.artist + "\x1F" + it.title;
        auto found = songIndex.find(key);
        if (found == songIndex.end()) {
            songIndex[key] = songs.size();
            songs.push_back({it.title, it.artist, it.seconds, it.plays});
        } else {
            songs[found->second].seconds += it.seconds;
            songs[found->second].plays += it.plays;
        }
        if (!it.artist.empty()) {
            Entry& a = artists[it.artist];
            a.artist = it.artist;
            a.seconds += it.seconds;
            a.plays += it.plays;
        }
    }
    s.songs = static_cast<int>(songs.size());
    s.artists = static_cast<int>(artists.size());
    std::sort(songs.begin(), songs.end(), [](const Entry& a, const Entry& b) { return a.seconds > b.seconds; });
    for (std::size_t i = 0; i < songs.size() && i < 5; ++i) s.topSongs.push_back(songs[i]);
    std::vector<Entry> list;
    for (auto& [name, e] : artists) list.push_back(e);
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.seconds > b.seconds; });
    for (std::size_t i = 0; i < list.size() && i < 5; ++i) s.topArtists.push_back(list[i]);
    return s;
}

Summary FromServer(const json& j, int year) {
    Summary s;
    s.year = year;
    s.fromCloud = true;
    s.seconds = j.value("seconds", 0.0);
    s.plays = j.value("plays", 0);
    s.songs = j.value("songs", 0);
    s.artists = j.value("artists", 0);
    auto entries = [&](const char* key, std::vector<Entry>& out) {
        if (!j.contains(key) || !j[key].is_array()) return;
        for (const json& e : j[key]) {
            if (!e.is_object()) continue;
            out.push_back({e.value("title", std::string()), e.value("artist", std::string()), e.value("seconds", 0.0), e.value("plays", 0)});
        }
    };
    if (j.contains("share") && j["share"].is_object()) {
        s.shareUrl = j["share"].value("url", std::string());
        s.shareShowName = j["share"].value("showName", false);
    }
    entries("topSongs", s.topSongs);
    entries("topArtists", s.topArtists);
    if (j.contains("months") && j["months"].is_array()) {
        for (std::size_t i = 0; i < 12 && i < j["months"].size(); ++i) {
            if (j["months"][i].is_number()) s.months[i] = j["months"][i].get<double>();
        }
    }
    return s;
}

// --- The server --------------------------------------------------------------------------
struct Reply {
    int status = 0;
    json body = json::object();
};

Reply Call(const wchar_t* method, const std::wstring& path, const std::string& pass, const std::string& body = {}) {
    Reply reply;
    std::wstring headers = L"Accept: application/json\r\nAuthorization: Bearer " + net::Widen(pass) + L"\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
    net::Response r;
    if (!net::Request(method, account::ApiBase() + path, body, headers, r, 1024 * 1024)) return reply;
    reply.status = r.status;
    reply.body = json::parse(r.body, nullptr, false);
    if (reply.body.is_discarded() || !reply.body.is_object()) reply.body = json::object();
    return reply;
}

const char* FailKey(int status) {
    return status == 0 ? "year.err_offline" : status == 401 ? "year.err_session" : "year.err_server";
}

// Sends what changed since the last time (up to 200 songs per request).
// Returns false if it couldn't.
bool Upload(const std::string& pass) {
    for (int round = 0; round < 20; ++round) {
        json items = json::array();
        std::vector<std::pair<std::size_t, std::pair<long long, int>>> marks;
        {
            std::lock_guard lock(g_mutex);
            for (std::size_t i = 0; i < g_items.size() && items.size() < 200; ++i) {
                const Item& it = g_items[i];
                const long long seconds = static_cast<long long>(it.seconds) - it.sentSeconds;
                const int plays = it.plays - it.sentPlays;
                if (seconds <= 0 && plays <= 0) continue;
                items.push_back({{"y", it.y}, {"m", it.m}, {"artist", it.artist}, {"title", it.title},
                                 {"seconds", std::max(0LL, seconds)}, {"plays", std::max(0, plays)}});
                marks.push_back({i, {static_cast<long long>(it.seconds), it.plays}});
            }
        }
        if (items.empty()) break;
        const Reply r = Call(L"POST", L"/v1/me/plays", pass, json{{"items", std::move(items)}}.dump());
        std::lock_guard lock(g_mutex);
        if (r.status != 200) {
            g_info.uploadKey = FailKey(r.status);
            ++g_info.version;
            return false;
        }
        for (const auto& [i, sent] : marks) {
            if (i < g_items.size()) {
                g_items[i].sentSeconds = sent.first;
                g_items[i].sentPlays = sent.second;
            }
        }
        g_dirty = true;
    }
    std::lock_guard lock(g_mutex);
    g_info.uploadKey.clear();
    g_info.lastUpload = static_cast<long long>(std::time(nullptr));
    ++g_info.version;
    return true;
}

void FetchCloud(const std::string& pass, int year) {
    const Reply r = Call(L"GET", L"/v1/me/wrapped?year=" + std::to_wstring(year), pass);
    std::lock_guard lock(g_mutex);
    g_info.cloudLoading = false;
    ++g_info.version;
    if (r.status != 200) return;
    g_cloud = FromServer(r.body, year);
    g_cloudOk = true;
}

void Finish(const char* key, bool ok) {
    std::lock_guard lock(g_mutex);
    g_info.actionKey = key;
    g_info.actionOk = ok;
    ++g_info.version;
}

enum class Job { None, DeleteCloud, Share, Unshare };
Job g_job = Job::None;
int g_jobYear = 0;
bool g_jobShowName = false;

// --- The thread -------------------------------------------------------------------------
void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Clock::time_point last = Clock::now();
    Clock::time_point lastSave = last;
    Clock::time_point nextUpload = last + std::chrono::seconds(20);  // (a moment after opening)
    std::uint32_t trackId = 0;
    double heard = 0.0;      // seconds of the current song (for "one play")
    bool counted = false;
    bool wanting = false;
    for (;;) {
        Job job = Job::None;
        int cloudYear = 0;
        bool uploadNow = false;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait_for(lock, kTick, [] { return g_quit || g_job != Job::None || g_uploadNow || g_cloudWanted != 0; });
            if (g_quit) break;
            job = g_job;
            g_job = Job::None;
            cloudYear = g_cloudWanted;
            g_cloudWanted = 0;
            uploadNow = g_uploadNow;
            g_uploadNow = false;
        }
        const Clock::time_point now = Clock::now();
        const double dt = std::min(10.0, std::chrono::duration<double>(now - last).count());
        last = now;
        const bool game = g_gameRunning.load();
        bool recording = false;
        {
            std::lock_guard lock(g_mutex);
            recording = g_recording;
        }
        // Only listen to Windows' media while the game is open (and recording).
        const bool want = game && recording;
        if (want != wanting) {
            media::Want(media::kHud, want);  // (the launcher never uses kHud for anything else)
            wanting = want;
        }
        std::string nowTitle;
        if (want) {
            const media::Track t = media::Current();
            const std::string title = Trim(t.title, 160);
            if (t.present && t.playing && !title.empty()) {
                if (t.trackId != trackId) {
                    trackId = t.trackId;
                    heard = 0.0;
                    counted = false;
                }
                heard += dt;
                std::time_t clock = std::time(nullptr);
                std::tm local{};
                localtime_s(&local, &clock);
                const int y = local.tm_year + 1900, m = local.tm_mon + 1;
                const std::string artist = Trim(t.artist, 160);
                std::lock_guard lock(g_mutex);
                const std::string key = Key(y, m, artist, title);
                auto it = g_index.find(key);
                if (it == g_index.end() && g_items.size() < kMaxItems) {
                    Item item;
                    item.y = y;
                    item.m = m;
                    item.artist = artist;
                    item.title = title;
                    g_items.push_back(std::move(item));
                    it = g_index.emplace(key, g_items.size() - 1).first;
                }
                if (it != g_index.end()) {
                    Item& item = g_items[it->second];
                    item.seconds += dt;
                    if (!counted && heard >= kHeardAfter) {
                        ++item.plays;
                        counted = true;
                    }
                    g_dirty = true;
                }
                nowTitle = title;
            }
        }
        {
            std::lock_guard lock(g_mutex);
            if (g_info.gameRunning != game || g_info.nowTitle != nowTitle) {
                g_info.gameRunning = game;
                g_info.nowTitle = nowTitle;
                ++g_info.version;
            }
            if (g_dirty && (now - lastSave >= kSaveEvery || job != Job::None)) {
                Save();
                lastSave = now;
            }
        }
        // The account: upload, the cloud's summary, deleting.
        const std::string pass = account::LumaPass();
        if (job == Job::DeleteCloud) {
            if (pass.empty()) {
                Finish("year.err_signed_out", false);
            } else {
                const Reply r = Call(L"DELETE", L"/v1/me/plays", pass);
                if (r.status == 200) {
                    std::lock_guard lock(g_mutex);
                    g_upload = false;
                    g_info.upload = false;
                    for (Item& it : g_items) {
                        it.sentSeconds = 0;  // (if you turn it on again, this PC's history goes up again)
                        it.sentPlays = 0;
                    }
                    g_cloudOk = false;
                    Save();
                }
                Finish(r.status == 200 ? "year.deleted_cloud" : FailKey(r.status), r.status == 200);
            }
        }
        if (job == Job::Share || job == Job::Unshare) {
            int year = 0;
            bool showName = false;
            {
                std::lock_guard lock(g_mutex);
                year = g_jobYear;
                showName = g_jobShowName;
            }
            Reply r;
            if (!pass.empty()) {
                r = job == Job::Share ? Call(L"POST", L"/v1/me/wrapped/share", pass, json{{"year", year}, {"showName", showName}}.dump())
                                      : Call(L"DELETE", L"/v1/me/wrapped/share?year=" + std::to_wstring(year), pass);
            }
            std::lock_guard lock(g_mutex);
            g_info.shareBusy = false;
            ++g_info.version;
            if (pass.empty()) {
                g_info.actionKey = "year.err_signed_out";
                g_info.actionOk = false;
            } else if (r.status == 200) {
                const std::string url = job == Job::Share ? r.body.value("url", std::string()) : std::string();
                if (g_cloudOk && g_cloud.year == year) {
                    g_cloud.shareUrl = url;
                    g_cloud.shareShowName = showName;
                }
                if (job == Job::Share) {
                    g_info.linkUrl = url;
                    ++g_info.linkVersion;
                }
                g_info.actionKey = job == Job::Share ? "year.link_copied" : "year.link_removed";
                g_info.actionOk = true;
            } else {
                g_info.actionKey = r.body.value("error", std::string()) == "nothing_to_share" ? "year.err_nothing" : FailKey(r.status);
                g_info.actionOk = false;
            }
        }
        bool upload = false;
        {
            std::lock_guard lock(g_mutex);
            upload = g_upload;
            // Another account: what was "sent" was sent to the other one.
            const std::string who = account::Get().gamertag;
            if (upload && !pass.empty() && !who.empty() && who != g_account) {
                g_account = who;
                for (Item& it : g_items) {
                    it.sentSeconds = 0;
                    it.sentPlays = 0;
                }
                g_dirty = true;
                uploadNow = true;
            }
        }
        if (upload && !pass.empty() && (uploadNow || now >= nextUpload)) {
            nextUpload = now + (Upload(pass) ? kUploadEvery : kRetry);
            std::lock_guard lock(g_mutex);
            if (g_dirty) Save();
        }
        if (cloudYear != 0 && !pass.empty()) FetchCloud(pass, cloudYear);
        else if (cloudYear != 0) {
            std::lock_guard lock(g_mutex);
            g_info.cloudLoading = false;
        }
    }
    if (wanting) media::Want(media::kHud, false);
    {
        std::lock_guard lock(g_mutex);
        if (g_dirty) Save();
    }
    CoUninitialize();
}

}  // namespace

void Start() {
    std::lock_guard lock(g_mutex);
    Load();
    g_info.recording = g_recording;
    g_info.upload = g_upload;
    g_quit = false;
    g_thread = std::thread(Run);
}

void Stop() {
    {
        std::lock_guard lock(g_mutex);
        g_quit = true;
    }
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void SetGameRunning(bool running) {
    g_gameRunning = running;
}

Info Get() {
    std::lock_guard lock(g_mutex);
    return g_info;
}

void SetRecording(bool on) {
    std::lock_guard lock(g_mutex);
    g_recording = on;
    g_info.recording = on;
    ++g_info.version;
    Save();
}

void SetUpload(bool on) {
    {
        std::lock_guard lock(g_mutex);
        g_upload = on;
        g_info.upload = on;
        g_info.uploadKey.clear();
        g_uploadNow = on;
        ++g_info.version;
        Save();
    }
    g_wake.notify_all();
}

Summary Local(int year) {
    std::lock_guard lock(g_mutex);
    return Summarize(year);
}

Summary Cloud(int year, bool& ok) {
    std::lock_guard lock(g_mutex);
    ok = g_cloudOk && g_cloud.year == year;
    return ok ? g_cloud : Summary{};
}

void RefreshCloud(int year) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.cloudLoading) return;
        g_info.cloudLoading = true;
        g_cloudWanted = year;
    }
    g_wake.notify_all();
}

void DeleteLocal() {
    std::lock_guard lock(g_mutex);
    g_items.clear();
    g_index.clear();
    Save();
    g_info.actionKey = "year.deleted_local";
    g_info.actionOk = true;
    ++g_info.version;
}

void DeleteCloud() {
    {
        std::lock_guard lock(g_mutex);
        g_job = Job::DeleteCloud;
    }
    g_wake.notify_all();
}

void Share(int year, bool showName) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.shareBusy) return;
        g_info.shareBusy = true;
        g_job = Job::Share;
        g_jobYear = year;
        g_jobShowName = showName;
    }
    g_wake.notify_all();
}

void Unshare(int year) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.shareBusy) return;
        g_info.shareBusy = true;
        g_job = Job::Unshare;
        g_jobYear = year;
    }
    g_wake.notify_all();
}

}  // namespace luma::launcher::listening
