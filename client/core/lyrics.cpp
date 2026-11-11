// =============================================================================
//  lyrics.cpp - The lyrics of the song that's playing (see lyrics.h)
// =============================================================================
#include "core/lyrics.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <iterator>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/media.h"
#include "core/paths.h"
#include "core/version.h"

namespace luma::lyrics {

namespace {

std::atomic<std::uint32_t> g_wanted{0};
std::atomic<bool> g_online{true};
std::atomic<bool> g_hdCovers{true};
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::mutex g_threadMutex;

std::atomic<bool> g_reached{false};  // did any source answer (even "not found")?

std::mutex g_mutex;
Result g_result;
std::uint32_t g_version = 0;

void Publish(Result result) {
    std::lock_guard lock(g_mutex);
    g_result = std::move(result);
    ++g_version;
}

// --- Small helpers ---------------------------------------------------------------------

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char hex[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

// One HTTPS GET. Returns the status code (0 = no connection).
int HttpGet(const wchar_t* host, const std::string& pathAndQuery, std::string& body) {
    const std::wstring agent = Widen(std::string("Luma/") + kVersion + " (Minecraft Bedrock client)");
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 6000, 6000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    const std::wstring path = Widen(pathAndQuery);
    HINTERNET request = connection != nullptr ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                              : nullptr;
    int status = 0;
    if (request != nullptr) {
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0;
            DWORD size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = static_cast<int>(code);
            if (status > 0) g_reached.store(true);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0 || body.size() > 1024 * 1024) break;
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
                body.append(chunk.data(), read);
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status;
}

// GET of a full "https://host/path" URL.
int HttpGetUrl(const std::string& url, std::string& body) {
    const std::string prefix = "https://";
    if (url.compare(0, prefix.size(), prefix) != 0) return 0;
    const std::size_t slash = url.find('/', prefix.size());
    const std::string host = url.substr(prefix.size(), slash == std::string::npos ? std::string::npos : slash - prefix.size());
    const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    return HttpGet(Widen(host).c_str(), path, body);
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "Song (feat. X) - Remastered 2011" -> "Song": a second, looser try.
std::string CleanTitle(std::string t) {
    for (const char* cut : {" (feat", " (ft", " [", " - Remaster", " - Live", " - Radio", " - From", " (Remaster",
                            " - Single", " (Official", " (Lyric", " (Audio"}) {
        const std::size_t at = t.find(cut);
        if (at != std::string::npos && at > 0) t.erase(at);
    }
    return t;
}

// --- LRC parsing -----------------------------------------------------------------------
// "[01:02.34] text" (several [..] = the line repeats) and, for word timings,
// "enhanced LRC": "[01:02.34] <01:02.34> Hello <01:02.90> world".

bool ParseTime(const char* s, int len, int& ms) {
    int minutes = 0, seconds = 0, fraction = 0, digits = 0;
    if (sscanf_s(s, "%d:%d", &minutes, &seconds) < 2) return false;
    const char* dot = static_cast<const char*>(std::memchr(s, '.', static_cast<std::size_t>(len)));
    if (dot == nullptr) dot = static_cast<const char*>(std::memchr(s, ',', static_cast<std::size_t>(len)));
    if (dot != nullptr) {
        for (const char* p = dot + 1; p < s + len && std::isdigit(static_cast<unsigned char>(*p)) && digits < 3; ++p, ++digits) {
            fraction = fraction * 10 + (*p - '0');
        }
        while (digits < 3) {
            fraction *= 10;
            ++digits;
        }
    }
    ms = (minutes * 60 + seconds) * 1000 + fraction;
    return true;
}

std::vector<Line> ParseLrc(const std::string& lrc, bool& wordSynced) {
    std::vector<Line> lines;
    wordSynced = false;
    std::istringstream in(lrc);
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        std::vector<int> times;
        std::size_t i = 0;
        while (i < raw.size() && raw[i] == '[') {
            const std::size_t close = raw.find(']', i);
            if (close == std::string::npos) break;
            int ms = 0;
            if (ParseTime(raw.c_str() + i + 1, static_cast<int>(close - i - 1), ms)) times.push_back(ms);
            i = close + 1;
        }
        if (times.empty()) continue;  // metadata ([ar:...]) or garbage
        std::string rest = raw.substr(i);
        Line line;
        // Word timings?
        if (rest.find('<') != std::string::npos) {
            std::size_t j = 0;
            while (j < rest.size()) {
                const std::size_t open = rest.find('<', j);
                if (open == std::string::npos) break;
                const std::size_t close = rest.find('>', open);
                if (close == std::string::npos) break;
                int ms = 0;
                const bool ok = ParseTime(rest.c_str() + open + 1, static_cast<int>(close - open - 1), ms);
                const std::size_t next = rest.find('<', close);
                std::string word = rest.substr(close + 1, next == std::string::npos ? std::string::npos : next - close - 1);
                if (ok && !word.empty()) line.words.push_back(Word{ms, word});
                j = next == std::string::npos ? rest.size() : next;
            }
            for (const Word& w : line.words) line.text += w.text;
        } else {
            line.text = rest;
        }
        while (!line.text.empty() && line.text.front() == ' ') line.text.erase(line.text.begin());
        if (!line.words.empty()) {
            wordSynced = true;
            while (!line.words.front().text.empty() && line.words.front().text.front() == ' ') {
                line.words.front().text.erase(line.words.front().text.begin());
            }
        }
        for (int t : times) {
            line.ms = t;
            lines.push_back(line);
        }
    }
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.ms < b.ms; });
    return lines;
}

// --- The found lyrics, as saved on disk and as built from a source --------------------
struct Found {
    std::string source;
    std::string synced;   // LRC (may be empty)
    std::string plain;    // plain text (may be empty)
    bool instrumental = false;
};

Result ToResult(const Found& f, std::uint32_t trackId) {
    Result r;
    r.trackId = trackId;
    r.source = f.source;
    if (f.instrumental) {
        r.status = Status::Instrumental;
        return r;
    }
    if (!f.synced.empty()) {
        bool words = false;
        r.lines = ParseLrc(f.synced, words);
        r.synced = !r.lines.empty();
        r.wordSynced = words;
    }
    if (r.lines.empty() && !f.plain.empty()) {
        std::istringstream in(f.plain);
        std::string raw;
        while (std::getline(in, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            r.lines.push_back(Line{0, raw, {}});
        }
    }
    r.status = r.lines.empty() ? Status::NotFound : Status::Found;
    return r;
}

// --- Sources -----------------------------------------------------------------------------
struct Query {
    std::string title, artist, album;
    int seconds = 0;
};

// LRCLIB: exact match first (title + artist + album + length), then a search.
bool FromLrclib(const Query& q, Found& out) {
    auto fill = [&](const nlohmann::json& j) {
        out.source = "LRCLIB";
        out.instrumental = j.value("instrumental", false);
        if (j.contains("syncedLyrics") && j["syncedLyrics"].is_string()) out.synced = j["syncedLyrics"].get<std::string>();
        if (j.contains("plainLyrics") && j["plainLyrics"].is_string()) out.plain = j["plainLyrics"].get<std::string>();
        return out.instrumental || !out.synced.empty() || !out.plain.empty();
    };
    std::string body;
    std::string path = "/api/get?track_name=" + UrlEncode(q.title) + "&artist_name=" + UrlEncode(q.artist);
    if (!q.album.empty()) path += "&album_name=" + UrlEncode(q.album);
    if (q.seconds > 0) path += "&duration=" + std::to_string(q.seconds);
    if (HttpGet(L"lrclib.net", path, body) == 200) {
        const auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_object() && fill(j)) return true;
    }
    // Search: the closest length wins, synced ones first.
    for (const std::string& title : {q.title, CleanTitle(q.title)}) {
        body.clear();
        if (HttpGet(L"lrclib.net", "/api/search?track_name=" + UrlEncode(title) + "&artist_name=" + UrlEncode(q.artist),
                    body) != 200) {
            continue;
        }
        const auto list = nlohmann::json::parse(body, nullptr, false);
        if (!list.is_array() || list.empty()) continue;
        const nlohmann::json* best = nullptr;
        double bestScore = 1e9;
        for (const auto& item : list) {
            if (!item.is_object()) continue;
            const double d = item.value("duration", 0.0);
            double score = q.seconds > 0 && d > 0 ? std::fabs(d - q.seconds) : 30.0;
            if (!(item.contains("syncedLyrics") && item["syncedLyrics"].is_string())) score += 20.0;
            if (score < bestScore) {
                bestScore = score;
                best = &item;
            }
        }
        if (best != nullptr && bestScore < 40.0 && fill(*best)) return true;
    }
    return false;
}

struct LyricsSource {
    const char* name;
    bool (*fetch)(const Query&, Found&);
};
// In order of preference. To add a source: write its fetch and add it here.
constexpr LyricsSource kSources[] = {
    {"LRCLIB", &FromLrclib},
};

// --- Disk cache ----------------------------------------------------------------------------
std::filesystem::path CachePath(const Query& q) {
    std::uint64_t h = 1469598103934665603ull;
    const std::string key = q.artist + "\n" + q.title + "\n" + std::to_string(q.seconds / 3);
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.json", static_cast<unsigned long long>(h));
    return paths::DataDir() / L"lyrics" / name;
}

std::int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool LoadCache(const Query& q, Found& out) {
    std::ifstream in(CachePath(q), std::ios::binary);
    if (!in) return false;
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return false;
    const bool empty = j.value("synced", std::string()).empty() && j.value("plain", std::string()).empty() &&
                       !j.value("instrumental", false);
    // A song without lyrics is looked up again after 3 days (sources keep growing).
    if (empty && NowSeconds() - j.value("time", std::int64_t{0}) > 3 * 24 * 3600) return false;
    out.source = j.value("source", std::string());
    out.synced = j.value("synced", std::string());
    out.plain = j.value("plain", std::string());
    out.instrumental = j.value("instrumental", false);
    return true;
}

void SaveCache(const Query& q, const Found& f) {
    std::error_code ec;
    const auto path = CachePath(q);
    std::filesystem::create_directories(path.parent_path(), ec);
    const nlohmann::json j = {{"source", f.source}, {"title", q.title}, {"artist", q.artist},
                              {"synced", f.synced}, {"plain", f.plain}, {"instrumental", f.instrumental},
                              {"time", NowSeconds()}};
    std::ofstream out(path, std::ios::binary);
    out << j.dump();
}

// --- HD covers (Apple's iTunes catalog) ----------------------------------------------
std::filesystem::path CoverPath(const Query& q) {
    std::uint64_t h = 1469598103934665603ull;
    const std::string key = LowerAscii(q.artist) + "\n" + LowerAscii(q.title);
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.jpg", static_cast<unsigned long long>(h));
    return paths::DataDir() / L"covers" / name;
}

bool FetchCover(const Query& q, std::vector<std::uint8_t>& out) {
    {
        std::ifstream in(CoverPath(q), std::ios::binary);
        if (in) {
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!out.empty()) return true;
        }
    }
    const std::string title = CleanTitle(q.title);
    std::string body;
    if (HttpGet(L"itunes.apple.com", "/search?entity=song&limit=10&term=" + UrlEncode(q.artist + " " + title), body) != 200) {
        return false;
    }
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("results") || !j["results"].is_array()) return false;
    // The result whose title and artist match best.
    const std::string wantTitle = LowerAscii(title);
    const std::string wantArtist = LowerAscii(q.artist);
    std::string art;
    int bestScore = 0;
    for (const auto& r : j["results"]) {
        if (!r.is_object() || !r.contains("artworkUrl100") || !r["artworkUrl100"].is_string()) continue;
        const std::string track = LowerAscii(r.value("trackName", std::string()));
        const std::string artist = LowerAscii(r.value("artistName", std::string()));
        const std::string album = LowerAscii(r.value("collectionName", std::string()));
        int score = 0;
        if (track == wantTitle) score += 4;
        else if (track.find(wantTitle) != std::string::npos || wantTitle.find(track) != std::string::npos) score += 2;
        if (artist == wantArtist) score += 3;
        else if (!wantArtist.empty() && (artist.find(wantArtist.substr(0, wantArtist.find(' '))) != std::string::npos)) score += 1;
        if (!q.album.empty() && album == LowerAscii(q.album)) score += 1;
        if (score > bestScore) {
            bestScore = score;
            art = r["artworkUrl100"].get<std::string>();
        }
    }
    if (bestScore < 4 || art.empty()) return false;  // not sure it's the same song: keep the app's art
    // ".../100x100bb.jpg" -> ".../1000x1000bb.jpg" (Apple serves any size from the same picture).
    const std::size_t at = art.rfind("100x100");
    if (at != std::string::npos) art.replace(at, 7, "1000x1000");
    std::string image;
    if (HttpGetUrl(art, image) != 200 || image.size() < 1000) return false;
    out.assign(image.begin(), image.end());
    std::error_code ec;
    std::filesystem::create_directories(CoverPath(q).parent_path(), ec);
    std::ofstream(CoverPath(q), std::ios::binary).write(image.data(), static_cast<std::streamsize>(image.size()));
    return true;
}

void ThreadMain() {
    std::uint32_t lastTrack = 0;
    std::uint32_t lastCoverTrack = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const std::uint32_t wanted = g_wanted.load();
        if (wanted == 0) continue;
        const media::Track t = media::Current();
        if (!t.present || t.title[0] == '\0') continue;
        // HD cover: once the app's own art was looked at, if it's small (or missing).
        if (t.trackId != lastCoverTrack && t.artChecked && (wanted & kCovers) != 0) {
            lastCoverTrack = t.trackId;
            if (g_hdCovers.load() && t.artSourceSize < 400) {
                Query q{t.title, t.artist, t.album, 0};
                std::vector<std::uint8_t> bytes;
                try {
                    if (FetchCover(q, bytes)) media::OfferArt(t.trackId, std::move(bytes));
                } catch (...) {
                }
            }
        }
        if ((wanted & (kMenu | kHud)) == 0 || t.trackId == lastTrack) continue;
        lastTrack = t.trackId;
        Query q{t.title, t.artist, t.album, static_cast<int>(std::lround(t.duration))};
        Result searching;
        searching.status = Status::Searching;
        searching.trackId = t.trackId;
        Publish(searching);
        Found found;
        bool ok = false;
        try {
            ok = LoadCache(q, found);
            if (!ok && g_online.load()) {
                g_reached.store(false);
                for (const LyricsSource& source : kSources) {
                    if (source.fetch(q, found)) {
                        ok = true;
                        break;
                    }
                }
                // Remember "not found" too (for 3 days), but only if a source
                // really answered: without internet, nothing is saved.
                if (!ok) found = Found{"", "", "", false};
                if (ok || g_reached.load()) SaveCache(q, found);
            }
        } catch (...) {
            ok = false;
        }
        Result r;
        if (ok) {
            r = ToResult(found, t.trackId);
        } else {
            r.trackId = t.trackId;
            r.status = g_online.load() ? Status::NotFound : Status::Offline;
        }
        log::Info("Lyrics: \"{}\" -> {} lines{}{} ({}).", t.title, r.lines.size(), r.synced ? ", synced" : "",
                  r.wordSynced ? " word by word" : "", r.source.empty() ? "none" : r.source);
        // Only publish if the song didn't change in the meantime.
        if (media::Current().trackId == t.trackId) Publish(std::move(r));
    }
}

void EnsureThread() {
    std::lock_guard lock(g_threadMutex);
    if (!g_thread.joinable() && !g_stop.load()) g_thread = std::thread(ThreadMain);
}

}  // namespace

void Want(Source source, bool want) {
    if (want) {
        g_wanted.fetch_or(source);
        EnsureThread();
    } else {
        g_wanted.fetch_and(~static_cast<std::uint32_t>(source));
    }
}

void SetOnline(bool online) {
    g_online.store(online);
}

void SetHdCovers(bool on) {
    g_hdCovers.store(on);
}

void Shutdown() {
    g_stop.store(true);
    std::lock_guard lock(g_threadMutex);
    if (g_thread.joinable()) g_thread.join();
}

bool Snapshot(std::uint32_t knownVersion, Result& out, std::uint32_t& version) {
    std::lock_guard lock(g_mutex);
    if (g_version == knownVersion) return false;
    out = g_result;
    version = g_version;
    return true;
}

int LineAt(const Result& lyrics, int ms) {
    if (!lyrics.synced) return -1;
    int index = -1;
    for (int i = 0; i < static_cast<int>(lyrics.lines.size()); ++i) {
        if (lyrics.lines[static_cast<std::size_t>(i)].ms <= ms) index = i;
        else break;
    }
    return index;
}

}  // namespace luma::lyrics
