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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
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
int HttpGet(const wchar_t* host, const std::string& pathAndQuery, std::string& body, int timeoutMs = 8000) {
    const std::wstring agent = Widen(std::string("Luma/") + kVersion + " (Minecraft Bedrock client)");
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return 0;
    WinHttpSetTimeouts(session, 6000, 6000, timeoutMs, timeoutMs);
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

// --- LiriQo ---------------------------------------------------------------------------------
// LiriQo (github.com/AlFarrizi-Studio/LiriQo) asks several catalogs at once
// (Apple Music, NetEase, LRCLIB, YouTube Music...) and returns the best
// lyrics it found, marked with their real sync level: "syllable"/"word"
// (karaoke), "line" or "plain". It never makes up word times.
// It's run by one person on their own server: if it's down or doesn't know
// the song, the next source (LRCLIB) is asked. It can be turned off.

std::atomic<bool> g_liriqo{true};

std::string FormatLrcTime(int ms) {
    if (ms < 0) ms = 0;
    char text[24];
    std::snprintf(text, sizeof(text), "%02d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return text;
}

// The value of attribute "name" inside an XML tag ("<p begin=\"1.5\" ...>").
std::string XmlAttribute(const std::string& tag, const char* name) {
    const std::string key = std::string(" ") + name + "=\"";
    const std::size_t at = tag.find(key);
    if (at == std::string::npos) return {};
    const std::size_t start = at + key.size();
    const std::size_t end = tag.find('"', start);
    return end == std::string::npos ? std::string() : tag.substr(start, end - start);
}

// TTML times: "27.395", "1:02.5" or "1:02:03.5" -> milliseconds.
int TtmlTime(const std::string& t) {
    if (t.empty()) return -1;
    double total = 0.0;
    std::size_t start = 0;
    for (;;) {
        const std::size_t colon = t.find(':', start);
        const double part = std::atof(t.substr(start, colon == std::string::npos ? std::string::npos : colon - start).c_str());
        total = total * 60.0 + part;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return static_cast<int>(std::lround(total * 1000.0));
}

std::string XmlText(std::string s) {
    const std::pair<const char*, const char*> entities[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                                                            {"&apos;", "'"}, {"&#39;", "'"}};
    for (const auto& [from, to] : entities) {
        for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + 1)) {
            s.replace(at, std::strlen(from), to);
        }
    }
    return s;
}

// Apple Music's TTML -> enhanced LRC ("[00:27.395]<00:27.395>I <00:27.549>been ...").
// TTML is the only place that says whether two pieces are separate words
// ("long e nough" is really "long" + "e"+"nough", one word in two
// syllables): a space between two <span> = a new word; no space = the same
// word. Background vocals (role "x-bg") are left out for now.
std::string TtmlToLrc(const std::string& ttml) {
    std::string lrc;
    std::size_t pos = 0;
    while ((pos = ttml.find("<p ", pos)) != std::string::npos) {
        const std::size_t tagEnd = ttml.find('>', pos);
        const std::size_t close = ttml.find("</p>", pos);
        if (tagEnd == std::string::npos || close == std::string::npos) break;
        const int lineMs = TtmlTime(XmlAttribute(ttml.substr(pos, tagEnd - pos), "begin"));
        std::string line;
        bool anyWord = false;
        int depthSkip = 0;  // inside a background-vocals span
        std::size_t i = tagEnd + 1;
        while (i < close) {
            if (ttml[i] != '<') {
                // Text between spans: a space here separates words.
                const std::size_t next = ttml.find('<', i);
                const std::string between = ttml.substr(i, std::min(next, close) - i);
                if (depthSkip == 0 && anyWord && between.find(' ') != std::string::npos && (line.empty() || line.back() != ' ')) {
                    line += ' ';
                }
                i = std::min(next, close);
                continue;
            }
            const std::size_t end = ttml.find('>', i);
            if (end == std::string::npos || end > close) break;
            const std::string tag = ttml.substr(i, end - i + 1);
            if (tag.compare(0, 6, "<span ") == 0 || tag == "<span>") {
                if (depthSkip > 0 || tag.find("x-bg") != std::string::npos) {
                    ++depthSkip;
                } else {
                    // A timed piece: "<span begin=.. end=..>text</span>" (no nested tags).
                    const std::size_t textEnd = ttml.find('<', end + 1);
                    const int ms = TtmlTime(XmlAttribute(tag, "begin"));
                    if (textEnd != std::string::npos && ms >= 0 && ttml.compare(textEnd, 7, "</span>") == 0) {
                        const std::string text = XmlText(ttml.substr(end + 1, textEnd - end - 1));
                        line += "<" + FormatLrcTime(ms) + ">" + text;
                        anyWord = true;
                        i = textEnd + 7;
                        continue;
                    }
                }
            } else if (tag == "</span>" && depthSkip > 0) {
                --depthSkip;
            }
            i = end + 1;
        }
        if (anyWord && lineMs >= 0) {
            while (!line.empty() && line.back() == ' ') line.pop_back();
            lrc += "[" + FormatLrcTime(lineMs) + "]" + line + "\n";
        }
        pos = close + 4;
    }
    return lrc;
}

// Word times without TTML (NetEase, Kuwo...): the "timed" lines of the JSON.
//
// THE PROBLEM: these sources split words into syllables ("Elys" + "é" + "es")
// and LiriQo gives the pieces without their spaces, so we can't tell
// "Elys"+"ées" (one word) from "Champs" "Elysées" (two words).
// THE SOLUTION: LiriQo also brings line-synced versions of the same song
// (LRCLIB, YouTube Music), whose text HAS the real spaces and punctuation
// ("Aux Champs-Elysées"). We lay the pieces over that text, letter by
// letter: each piece takes its letters plus the spaces/punctuation after
// them. If the letters don't match (another spelling), we fall back to
// a space between pieces.

struct RefLine {
    int ms = 0;
    std::string text;
};

// The lines (time + text) of every line-synced version LiriQo brought.
std::vector<RefLine> ReferenceLines(const nlohmann::json& response) {
    std::vector<RefLine> refs;
    if (!response.contains("tracks") || !response["tracks"].is_array()) return refs;
    for (const auto& track : response["tracks"]) {
        if (!track.is_object() || track.value("syncLevel", std::string()) != "line") continue;
        if (!track.contains("timed") || !track["timed"].is_array()) continue;
        for (const auto& line : track["timed"]) {
            if (!line.is_object()) continue;
            const std::string text = line.value("text", std::string());
            if (!text.empty()) refs.push_back(RefLine{line.value("start", 0), text});
        }
    }
    return refs;
}

char FoldAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// A byte that's part of a word: a letter/number or any non-ASCII byte (é, ü, 日...).
bool IsWordByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u);
}

// Lays "pieces" over "ref". On success, "out" has each piece with its real
// spaces and punctuation ("Champs-", "Elys", "é", "es ").
bool AlignPieces(const std::string& ref, const std::vector<std::string>& pieces, std::vector<std::string>& out) {
    std::vector<std::size_t> starts;
    std::size_t r = 0;
    for (const std::string& piece : pieces) {
        std::size_t first = std::string::npos;
        for (char c : piece) {
            if (c == ' ' || c == '\t') continue;
            // Skip the reference's spaces and punctuation the piece doesn't have.
            while (r < ref.size() && FoldAscii(ref[r]) != FoldAscii(c) && !IsWordByte(ref[r])) ++r;
            if (r >= ref.size() || FoldAscii(ref[r]) != FoldAscii(c)) return false;
            if (first == std::string::npos) first = r;
            ++r;
        }
        if (first == std::string::npos) return false;
        starts.push_back(first);
    }
    // Only punctuation may be left over (a "!" at the end), not words.
    for (std::size_t i = r; i < ref.size(); ++i) {
        if (std::isalnum(static_cast<unsigned char>(ref[i]))) return false;
    }
    out.clear();
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::size_t from = i == 0 ? 0 : starts[i];
        const std::size_t to = i + 1 < starts.size() ? starts[i + 1] : ref.size();
        out.push_back(ref.substr(from, to - from));
    }
    while (!out.empty() && !out.front().empty() && out.front().front() == ' ') out.front().erase(out.front().begin());
    return true;
}

std::string TimedToLrc(const nlohmann::json& timed, const std::vector<RefLine>& refs) {
    std::string lrc;
    std::vector<std::string> pieces, aligned;
    std::vector<int> times;
    for (const auto& line : timed) {
        if (!line.is_object() || !line.contains("words") || !line["words"].is_array() || line["words"].empty()) continue;
        pieces.clear();
        times.clear();
        for (const auto& w : line["words"]) {
            if (!w.is_object() || w.value("isBackground", false)) continue;
            const std::string piece = w.value("text", std::string());
            if (piece.empty()) continue;
            pieces.push_back(piece);
            times.push_back(w.value("start", 0));
        }
        if (pieces.empty()) continue;
        const int lineMs = line.value("start", times.front());
        // The reference line: close in time (2.5 s) and whose letters match.
        bool ok = false;
        for (const RefLine& ref : refs) {
            if (std::abs(ref.ms - lineMs) <= 2500 && AlignPieces(ref.text, pieces, aligned)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            // No reference: a space between pieces (except CJK, which has none).
            aligned.clear();
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                const bool cjk = static_cast<unsigned char>(pieces[i].front()) >= 0xE0;  // 3-byte UTF-8: CJK and the like
                aligned.push_back(pieces[i] + (i + 1 < pieces.size() && !cjk && pieces[i].back() != ' ' ? " " : ""));
            }
        }
        std::string text;
        for (std::size_t i = 0; i < aligned.size(); ++i) text += "<" + FormatLrcTime(times[i]) + ">" + aligned[i];
        lrc += "[" + FormatLrcTime(lineMs) + "]" + text + "\n";
    }
    return lrc;
}

// Do these look like the same song? LiriQo may answer with its closest
// match; better nothing than the lyrics of another song.
bool SameSong(const Query& q, const nlohmann::json& track) {
    const std::string artist = LowerAscii(track.value("artist", std::string()));
    const std::string title = LowerAscii(track.value("title", std::string()));
    const std::string wantArtist = LowerAscii(q.artist);
    const std::string wantTitle = LowerAscii(CleanTitle(q.title));
    if (title.empty() || wantTitle.empty()) return false;
    const bool titleOk = title.find(wantTitle) != std::string::npos || wantTitle.find(LowerAscii(CleanTitle(title))) != std::string::npos;
    // The artist: any word of 3+ letters in common ("The Weeknd" / "The Weeknd, Daft Punk").
    bool artistOk = wantArtist.empty();
    std::istringstream words(wantArtist);
    std::string word;
    while (!artistOk && words >> word) {
        if (word.size() >= 3 && word != "the" && artist.find(word) != std::string::npos) artistOk = true;
    }
    return titleOk && artistOk;
}

bool FromLiriqo(const Query& q, Found& out) {
    if (!g_liriqo.load()) return false;
    std::string path = "/v1/lyrics?title=" + UrlEncode(q.title) + "&artist=" + UrlEncode(q.artist);
    if (!q.album.empty()) path += "&album=" + UrlEncode(q.album);
    if (q.seconds > 0) path += "&duration=" + std::to_string(q.seconds);
    std::string body;
    // It asks many catalogs: it can take a few seconds (12 s at most).
    if (HttpGet(L"api.liriqo-alfarrizi.my.id", path, body, 12000) != 200) return false;
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("primary") || !j["primary"].is_object()) return false;
    const nlohmann::json& p = j["primary"];
    if (!SameSong(q, p)) {
        log::Info("Lyrics: LiriQo answered another song (\"{}\"), ignored.", p.value("title", std::string()));
        return false;
    }
    const std::string level = p.value("syncLevel", std::string());
    std::string synced;
    if (level == "syllable" || level == "word") {
        if (p.contains("ttml") && p["ttml"].is_string()) synced = TtmlToLrc(p["ttml"].get<std::string>());
        if (synced.empty() && p.contains("timed") && p["timed"].is_array()) synced = TimedToLrc(p["timed"], ReferenceLines(j));
    }
    if (synced.empty() && (level == "syllable" || level == "word" || level == "line") && p.contains("lrc") &&
        p["lrc"].is_string()) {
        synced = p["lrc"].get<std::string>();
    }
    std::string plain;
    if (p.contains("plain") && p["plain"].is_string()) plain = p["plain"].get<std::string>();
    if (synced.empty() && plain.empty()) return false;
    // A plain-text answer: maybe LRCLIB has it synced, so keep looking (it's
    // only used if nothing better appears).
    std::string upstream = p.value("upstreamProvider", p.value("source", std::string()));
    out.source = upstream.empty() ? "LiriQo" : upstream + " (LiriQo)";
    out.synced = synced;
    out.plain = plain;
    out.instrumental = false;
    return !synced.empty();
}


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
    // The same song may come with its length rounded differently (Windows
    // says 156.6 s, Spotify 156 s): try the neighbors too.
    std::ifstream in;
    for (int delta : {0, -1, 1}) {
        Query nearby = q;
        nearby.seconds = q.seconds + delta;
        in.open(CachePath(nearby), std::ios::binary);
        if (in) break;
        in.clear();
    }
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
    // Saved before LiriQo existed (or with it off) and without word times:
    // ask again, LiriQo may have them word by word.
    // Saved by an older Luma that joined syllables badly ("Elysé es"): ask again.
    if (out.source.find("(LiriQo)") != std::string::npos && j.value("format", 1) < 2) return false;
    if (g_liriqo.load() && !j.value("liriqo", false) && !out.instrumental &&
        out.synced.find('<') == std::string::npos) {
        return false;
    }
    return true;
}

void SaveCache(const Query& q, const Found& f) {
    std::error_code ec;
    const auto path = CachePath(q);
    std::filesystem::create_directories(path.parent_path(), ec);
    const nlohmann::json j = {{"source", f.source}, {"title", q.title}, {"artist", q.artist},
                              {"synced", f.synced}, {"plain", f.plain}, {"instrumental", f.instrumental},
                              {"time", NowSeconds()}, {"liriqo", g_liriqo.load()}, {"format", 2}};
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

// Looks a song up online (LiriQo and LRCLIB at once) and saves the result.
// "earlyTrack" != 0: publish LRCLIB's lyrics for that song while LiriQo
// works (0 = a prefetch: nothing is shown, it's only saved for later).
bool Lookup(const Query& q, Found& found, std::uint32_t earlyTrack, const char* title) {
    bool ok = false;
    g_reached.store(false);
    // BOTH SOURCES AT ONCE: LiriQo asks many catalogs and can take
    // 5-15 s; LRCLIB answers in 1-2 s. So LiriQo runs on a second
    // thread while we ask LRCLIB: LRCLIB's lyrics are shown right
    // away, and if LiriQo brings them word by word, they replace
    // them when they arrive.
    std::future<std::pair<bool, Found>> liriqo;
    if (g_liriqo.load()) {
        liriqo = std::async(std::launch::async, [q]() {
            Found f;
            bool r = false;
            try {
                r = FromLiriqo(q, f);
            } catch (...) {
            }
            return std::make_pair(r, f);
        });
    }
    Found lrclib;
    bool lrclibOk = false;
    try {
        lrclibOk = FromLrclib(q, lrclib);
    } catch (...) {
    }
    if (earlyTrack != 0 && lrclibOk && liriqo.valid() && media::Current().trackId == earlyTrack) {
        Result early = ToResult(lrclib, earlyTrack);
        log::Info("Lyrics: \"{}\" -> {} lines from LRCLIB first (waiting for LiriQo).", title, early.lines.size());
        Publish(std::move(early));
    }
    Found best;
    bool bestOk = false;
    if (liriqo.valid()) {
        // (It always finishes: its request gives up after 12 s.)
        auto [r, f] = liriqo.get();
        bestOk = r;
        best = std::move(f);
    }
    const bool liriqoWords = bestOk && best.synced.find('<') != std::string::npos;
    if (liriqoWords || (bestOk && !lrclibOk)) {
        found = best;  // word by word (or LRCLIB had nothing)
        ok = true;
    } else if (lrclibOk) {
        found = lrclib;
        ok = true;
    } else if (!best.plain.empty()) {
        found = best;  // only plain text, from LiriQo
        ok = true;
    }
    // Remember "not found" too (for 3 days), but only if a source
    // really answered: without internet, nothing is saved.
    if (!ok) found = Found{"", "", "", false};
    if (ok || g_reached.load()) SaveCache(q, found);
    return ok;
}

// --- Prefetch: the next songs of the queue (core/spotify) ---------------------------------
std::mutex g_prefetchMutex;
std::vector<Query> g_prefetch;  // waiting to be looked up (newest request first)

bool TakePrefetch(Query& out) {
    std::lock_guard lock(g_prefetchMutex);
    if (g_prefetch.empty()) return false;
    out = g_prefetch.front();
    g_prefetch.erase(g_prefetch.begin());
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
        if ((wanted & (kMenu | kHud)) == 0) continue;
        if (t.trackId == lastTrack) {
            // Nothing new playing: look up one of the next songs (if any).
            Query next;
            if (g_online.load() && TakePrefetch(next)) {
                Found cached;
                try {
                    if (!LoadCache(next, cached)) {
                        const bool ok = Lookup(next, cached, 0, next.title.c_str());
                        log::Info("Lyrics: prefetched \"{}\" ({}).", next.title, ok ? cached.source : std::string("none"));
                    }
                } catch (...) {
                }
            }
            continue;
        }
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
            if (!ok && g_online.load()) ok = Lookup(q, found, t.trackId, t.title);
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

void Prefetch(const std::string& title, const std::string& artist, const std::string& album, int seconds) {
    if (title.empty()) return;
    std::lock_guard lock(g_prefetchMutex);
    for (const Query& q : g_prefetch) {
        if (q.title == title && q.artist == artist) return;  // already waiting
    }
    if (g_prefetch.size() >= 6) g_prefetch.pop_back();
    g_prefetch.push_back(Query{title, artist, album, seconds});
}

void SetLiriqo(bool on) {
    g_liriqo.store(on);
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
