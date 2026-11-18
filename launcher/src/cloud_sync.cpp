// =============================================================================
//  cloud_sync.cpp - Your styles and settings in your Luma account (see cloud_sync.h)
// =============================================================================
#include "cloud_sync.h"

#include "account.h"
#include "net.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "flow_presets.h"

namespace luma::launcher::cloudsync {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace presets = luma::flowpresets;
using Clock = std::chrono::steady_clock;

constexpr auto kDebounce = std::chrono::seconds(5);     // send 5 s after the last change
constexpr auto kLook = std::chrono::seconds(2);         // look at the files every 2 s
constexpr auto kAskServer = std::chrono::minutes(2);    // ask "did another PC save?" every 2 min
constexpr auto kRetry = std::chrono::seconds(30);       // without internet: try again in 30 s

enum class Action { None, Export, DeleteCloud, DeleteAccount };

std::mutex g_mutex;          // guards g_info and g_action
std::condition_variable g_wake;
Info g_info;
Action g_action = Action::None;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_launcherReload{false};
std::thread g_thread;

// What this PC remembers (sync.json). Only the thread touches it, except "enabled".
struct Saved {
    bool enabled = false;
    std::string account;  // the gamertag it was synced with (lowercase)
    int rev = 0;          // the server's version this PC last saw (0 = never synced)
    std::string hash;     // fingerprint of what was last saved/brought (= what's in the cloud)
};
Saved g_saved;

// --- Files ---------------------------------------------------------------------------
fs::path DataDir() {
    PWSTR raw = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        path = fs::path(raw) / L"Luma";
    }
    CoTaskMemFree(raw);
    return path;
}

// Reads a JSON object. false = the file exists but is damaged (then it's never written over).
bool ReadObject(const fs::path& path, json& out) {
    out = json::object();
    std::error_code ec;
    if (!fs::exists(path, ec)) return true;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out = json::parse(file, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        out = json::object();
        return false;
    }
    return true;
}

// Written safely: to a .tmp, then it replaces the real file (whoever reads it
// at that moment sees the old file or the new one, never half).
bool WriteObject(const fs::path& path, const json& value) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = fs::path(path).concat(L".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << value.dump(4);
        if (!out) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// The active profile's file (as core/config.cpp names it).
fs::path ProfileFile(const fs::path& dir) {
    json root;
    ReadObject(dir / L"config" / L"luma.json", root);
    std::string name = "default";
    const auto it = root.find("activeProfile");
    if (it != root.end() && it->is_string()) name = it->get<std::string>();
    if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name.find("..") != std::string::npos) {
        name = "default";
    }
    name += ".json";
    return dir / L"config" / L"profiles" / fs::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
}

void LoadSaved() {
    json j;
    ReadObject(DataDir() / L"sync.json", j);
    Saved s;
    s.enabled = j.value("enabled", false);
    s.account = j.value("account", std::string());
    s.rev = j.value("rev", 0);
    s.hash = j.value("hash", std::string());
    g_saved = s;
}

void StoreSaved() {
    WriteObject(DataDir() / L"sync.json",
                json{{"enabled", g_saved.enabled}, {"account", g_saved.account}, {"rev", g_saved.rev}, {"hash", g_saved.hash}});
}

// --- What is synced ------------------------------------------------------------------
// The snapshot of this PC: {version, styles, nowPlaying, launcher}. false =
// something is damaged (then nothing is sent this time).
bool Snapshot(json& out) {
    const fs::path dir = DataDir();
    if (dir.empty()) return false;
    json styles = json::array();
    for (const presets::Preset& p : presets::Load(presets::FileIn(dir))) {
        if (!p.autoSaved) styles.push_back({{"name", p.name}, {"lyricFlow", p.look}});
    }
    json profile;
    if (!ReadObject(ProfileFile(dir), profile)) return false;
    json nowPlaying = json::object();
    const auto modules = profile.find("modules");
    if (modules != profile.end() && modules->is_object()) {
        const auto np = modules->find("now_playing");
        if (np != modules->end() && np->is_object()) nowPlaying = *np;
    }
    if (nowPlaying.contains("settings") && nowPlaying["settings"].is_object()) nowPlaying["settings"].erase("spotifyClientId");
    json launcherFile;
    ReadObject(dir / L"launcher.json", launcherFile);
    json launcher = {{"theme", launcherFile.value("theme", std::string("dark"))},
                     {"language", launcherFile.value("language", std::string("en"))},
                     {"sky", launcherFile.value("sky", 0)}};
    out = {{"version", 1}, {"styles", std::move(styles)}, {"nowPlaying", std::move(nowPlaying)}, {"launcher", std::move(launcher)}};
    return true;
}

// All numbers as decimals (the server may send 1.0 back as 1: same value).
void Normalize(json& j) {
    if (j.is_number()) {
        j = j.get<double>();
    } else if (j.is_object() || j.is_array()) {
        for (json& child : j) Normalize(child);
    }
}

// A fingerprint of a snapshot (the keys of a json object are always in the
// same order, so the same content always gives the same text). FNV-1a, 64 bits.
std::string Hash(const json& data) {
    json copy = data;
    if (copy.is_object()) copy.erase("version");
    Normalize(copy);
    const std::string text = copy.dump();
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", h);
    return out;
}

// Puts the cloud's copy on this PC. Only what changed is written.
void Apply(const json& data) {
    const fs::path dir = DataDir();
    if (dir.empty() || !data.is_object()) return;
    // 1. Your styles (the automatic "Your previous style" of this PC stays).
    const auto styles = data.find("styles");
    if (styles != data.end() && styles->is_array()) {
        const fs::path file = presets::FileIn(dir);
        const std::vector<presets::Preset> before = presets::Load(file);
        std::vector<presets::Preset> list;
        for (const json& s : *styles) {
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string() || !s.contains("lyricFlow")) continue;
            presets::Preset p{s["name"].get<std::string>(), presets::LookOf(s["lyricFlow"]), false};
            if (!p.name.empty() && !p.look.empty()) list.push_back(std::move(p));
        }
        for (const presets::Preset& p : before) {
            if (p.autoSaved) list.push_back(p);
        }
        bool same = before.size() == list.size();
        for (std::size_t i = 0; same && i < list.size(); ++i) {
            same = before[i].name == list[i].name && before[i].autoSaved == list[i].autoSaved && before[i].look == list[i].look;
        }
        if (!same) presets::Save(file, list);
    }
    // 2. Now Playing in the active profile (your Spotify Client ID stays the one of this PC).
    const auto np = data.find("nowPlaying");
    if (np != data.end() && np->is_object() && !np->empty()) {
        const fs::path file = ProfileFile(dir);
        json profile;
        if (ReadObject(file, profile)) {
            json& modules = profile["modules"];
            if (!modules.is_object()) modules = json::object();
            json fresh = *np;
            const json& old = modules["now_playing"];
            if (old.is_object() && old.contains("settings") && old["settings"].is_object() &&
                old["settings"].contains("spotifyClientId")) {
                if (!fresh["settings"].is_object()) fresh["settings"] = json::object();
                fresh["settings"]["spotifyClientId"] = old["settings"]["spotifyClientId"];
            }
            if (old != fresh) {
                modules["now_playing"] = std::move(fresh);
                WriteObject(file, profile);  // (the game, if open, notices and loads it)
            }
        }
    }
    // 3. The launcher's theme, language and sky.
    const auto l = data.find("launcher");
    if (l != data.end() && l->is_object()) {
        const fs::path file = dir / L"launcher.json";
        json launcher;
        if (ReadObject(file, launcher)) {
            json changed = launcher;
            for (const char* key : {"theme", "language", "sky"}) {
                if (l->contains(key)) changed[key] = (*l)[key];
            }
            if (changed != launcher) {
                WriteObject(file, changed);
                g_launcherReload = true;
            }
        }
    }
}

// The cloud's copy, plus your styles from this PC that it doesn't have (a
// style with the same name but another look gets a number: "Neon 2").
json Merge(const json& cloud, const json& local) {
    json out = cloud.is_object() ? cloud : json::object();
    if (!out.contains("styles") || !out["styles"].is_array()) out["styles"] = json::array();
    json& styles = out["styles"];
    if (local.contains("styles") && local["styles"].is_array()) {
        for (const json& s : local["styles"]) {
            if (styles.size() >= 100) break;
            const json look = presets::LookOf(s.value("lyricFlow", json::object()));
            bool have = false;
            for (const json& c : styles) have = have || presets::SameLook(presets::LookOf(c.value("lyricFlow", json::object())), look);
            if (have) continue;
            std::string name = s.value("name", std::string("Style"));
            auto taken = [&](const std::string& n) {
                for (const json& c : styles) {
                    if (c.value("name", std::string()) == n) return true;
                }
                return false;
            };
            if (taken(name)) {
                for (int n = 2;; ++n) {
                    const std::string candidate = name + " " + std::to_string(n);
                    if (!taken(candidate)) {
                        name = candidate;
                        break;
                    }
                }
            }
            styles.push_back({{"name", name}, {"lyricFlow", look}});
        }
    }
    // What the cloud doesn't have yet (an older copy): this PC's.
    for (const char* key : {"nowPlaying", "launcher"}) {
        if ((!out.contains(key) || !out[key].is_object() || out[key].empty()) && local.contains(key)) out[key] = local[key];
    }
    return out;
}

// --- The server ----------------------------------------------------------------------
enum class Result { Ok, Conflict, Offline, Session, Failed };

struct Reply {
    Result result = Result::Failed;
    json body;
};

Reply Call(const wchar_t* method, const wchar_t* path, const std::string& pass, const std::string& body = {}) {
    Reply reply;
    net::Response r;
    std::wstring headers = L"Authorization: Bearer " + net::Widen(pass) + L"\r\nAccept: application/json\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
    if (!net::Request(method, account::ApiBase() + path, body, headers, r, 1024 * 1024)) {
        reply.result = Result::Offline;
        return reply;
    }
    reply.body = json::parse(r.body, nullptr, false);
    if (reply.body.is_discarded()) reply.body = json::object();
    reply.result = r.status == 200 ? Result::Ok : r.status == 409 ? Result::Conflict : r.status == 401 ? Result::Session : Result::Failed;
    return reply;
}

void SetStatus(Status status, const char* errorKey = "") {
    std::lock_guard lock(g_mutex);
    g_info.status = status;
    g_info.errorKey = errorKey;
    if (status == Status::Synced) g_info.lastSync = static_cast<long long>(std::time(nullptr));
}

void SetFailure(Result result) {
    if (result == Result::Offline) SetStatus(Status::Offline);
    else if (result == Result::Session) SetStatus(Status::Error, "sync.err_session");
    else SetStatus(Status::Error, "sync.err_server");
}

// Sends this PC's snapshot as the new version. A conflict (another PC saved
// first) is solved with its copy, once.
bool Push(const std::string& pass, const json& local, int depth = 0);

// What to do with the cloud's copy (rev, data), compared with this PC.
bool Reconcile(const std::string& pass, int rev, const json& cloud, const json& local, int depth) {
    if (rev <= 0 || !cloud.is_object()) {
        g_saved.rev = 0;
        return Push(pass, local, depth);  // the cloud is empty: this PC's goes up
    }
    const bool localChanged = Hash(local) != g_saved.hash;
    if (g_saved.rev != 0 && rev == g_saved.rev) {
        if (localChanged) return Push(pass, local, depth);
        SetStatus(Status::Synced);
        return true;
    }
    // The cloud has a version this PC doesn't know (a new PC, or another PC saved).
    if (g_saved.rev != 0 && !localChanged) {
        Apply(cloud);  // nothing to lose here: just bring it
        json now;
        Snapshot(now);
        g_saved.rev = rev;
        g_saved.hash = Hash(now);
        StoreSaved();
        // If this PC's files can't hold exactly the cloud's copy (a damaged
        // profile...), the next look at the files will send them.
        SetStatus(Status::Synced);
        return true;
    }
    // First time on this PC, or both changed: the cloud's, plus this PC's styles.
    Apply(Merge(cloud, local));
    json now;
    if (!Snapshot(now)) now = Merge(cloud, local);
    g_saved.rev = rev;
    if (Hash(now) == Hash(cloud)) {
        g_saved.hash = Hash(now);
        StoreSaved();
        SetStatus(Status::Synced);
        return true;
    }
    return Push(pass, now, depth);
}

bool Push(const std::string& pass, const json& local, int depth) {
    SetStatus(Status::Working);
    const Reply r = Call(L"PUT", L"/v1/me/sync", pass, json{{"rev", g_saved.rev}, {"data", local}}.dump());
    if (r.result == Result::Ok) {
        g_saved.rev = r.body.value("rev", g_saved.rev + 1);
        g_saved.hash = Hash(local);
        StoreSaved();
        SetStatus(Status::Synced);
        return true;
    }
    if (r.result == Result::Conflict && depth == 0) {
        return Reconcile(pass, r.body.value("rev", 0), r.body.value("data", json()), local, 1);
    }
    SetFailure(r.result);
    return false;
}

bool Pull(const std::string& pass, const json& local) {
    SetStatus(Status::Working);
    const Reply r = Call(L"GET", L"/v1/me/sync", pass);
    if (r.result != Result::Ok) {
        SetFailure(r.result);
        return false;
    }
    return Reconcile(pass, r.body.value("rev", 0), r.body.value("data", json()), local, 0);
}

// --- Actions (the buttons of "Your data") -----------------------------------------------
void Finish(const char* key, bool ok, std::string path = {}) {
    std::lock_guard lock(g_mutex);
    g_info.actionBusy = false;
    g_info.actionKey = key;
    g_info.actionOk = ok;
    g_info.exportPath = std::move(path);
}

const char* FailureKey(Result result) {
    return result == Result::Offline ? "sync.err_offline" : result == Result::Session ? "sync.err_session" : "sync.err_server";
}

void TurnOff() {
    g_saved.enabled = false;
    g_saved.rev = 0;
    g_saved.hash.clear();
    StoreSaved();
    std::lock_guard lock(g_mutex);
    g_info.enabled = false;
    g_info.status = Status::Off;
    g_info.lastSync = 0;
}

void RunAction(Action action, const std::string& pass) {
    if (pass.empty()) {
        Finish("sync.err_signed_out", false);
        return;
    }
    if (action == Action::Export) {
        const Reply r = Call(L"GET", L"/v1/me/export", pass);
        if (r.result != Result::Ok) return Finish(FailureKey(r.result), false);
        PWSTR raw = nullptr;
        fs::path dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw) dir = raw;
        CoTaskMemFree(raw);
        if (dir.empty()) dir = DataDir();
        const fs::path file = dir / L"Luma - my data.json";
        if (!WriteObject(file, r.body)) return Finish("sync.err_write", false);
        const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        const std::u8string u8 = file.u8string();
        return Finish("sync.exported", true, std::string(u8.begin(), u8.end()));
    }
    if (action == Action::DeleteCloud) {
        const Reply r = Call(L"DELETE", L"/v1/me/sync", pass);
        if (r.result != Result::Ok) return Finish(FailureKey(r.result), false);
        TurnOff();
        return Finish("sync.deleted_cloud", true);
    }
    if (action == Action::DeleteAccount) {
        const Reply r = Call(L"DELETE", L"/v1/me", pass);
        if (r.result != Result::Ok) return Finish(FailureKey(r.result), false);
        TurnOff();
        account::SignOut();  // the session on this PC goes too
        return Finish("sync.deleted_account", true);
    }
}

// --- The thread ----------------------------------------------------------------------
void Run() {
    Clock::time_point lastLook{};
    Clock::time_point lastAsk{};
    Clock::time_point retryAt{};
    int passVersion = -1;
    std::string pendingHash;
    Clock::time_point pendingSince{};
    bool needAsk = true;
    while (!g_quit) {
        Action action = Action::None;
        bool enabled = false;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait_for(lock, std::chrono::seconds(1), [] { return g_quit.load() || g_action != Action::None; });
            if (g_quit) break;
            action = g_action;
            g_action = Action::None;
            enabled = g_info.enabled;
        }
        const std::string pass = account::LumaPass();
        if (action != Action::None) {
            RunAction(action, pass);
            needAsk = true;
            continue;
        }
        if (enabled != g_saved.enabled) {  // the switch changed
            g_saved.enabled = enabled;
            StoreSaved();
            needAsk = true;
            retryAt = {};
        }
        if (!enabled) {
            SetStatus(Status::Off);
            continue;
        }
        if (pass.empty()) {
            const account::State s = account::Get().state;
            SetStatus(s == account::State::SignedIn ? Status::Offline
                      : s == account::State::Refreshing || s == account::State::SigningIn ? Status::Working
                                                                                           : Status::NeedsAccount);
            needAsk = true;
            continue;
        }
        const Clock::time_point now = Clock::now();
        if (account::LumaPassVersion() != passVersion) {  // signed in (again), maybe with another account
            passVersion = account::LumaPassVersion();
            std::string who = account::Get().gamertag;
            for (char& c : who) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (who != g_saved.account) {
                g_saved.account = who;
                g_saved.rev = 0;  // another account: like a new PC
                g_saved.hash.clear();
                StoreSaved();
            }
            needAsk = true;
            retryAt = {};
        }
        if (now < retryAt) continue;
        if (!needAsk && now - lastLook < kLook && now - lastAsk < kAskServer) continue;
        lastLook = now;
        json local;
        if (!Snapshot(local)) {
            SetStatus(Status::Error, "sync.err_damaged");
            continue;
        }
        bool ok = true;
        if (needAsk || now - lastAsk >= kAskServer) {
            lastAsk = now;
            ok = Pull(pass, local);
            needAsk = !ok;
            pendingHash.clear();
        } else {
            const std::string h = Hash(local);
            if (h == g_saved.hash) {
                pendingHash.clear();
                continue;
            }
            if (h != pendingHash) {  // changed (again): wait until it stays still
                pendingHash = h;
                pendingSince = now;
                continue;
            }
            if (now - pendingSince < kDebounce) continue;
            ok = Push(pass, local);
            pendingHash.clear();
            if (!ok) needAsk = true;
        }
        if (!ok) retryAt = now + kRetry;
    }
}

}  // namespace

void Start() {
    LoadSaved();
    {
        std::lock_guard lock(g_mutex);
        g_info.enabled = g_saved.enabled;
        g_info.status = g_saved.enabled ? Status::Working : Status::Off;
    }
    g_quit = false;
    g_thread = std::thread(Run);
}

void Stop() {
    g_quit = true;
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

Info Get() {
    std::lock_guard lock(g_mutex);
    return g_info;
}

void SetEnabled(bool on) {
    {
        std::lock_guard lock(g_mutex);
        g_info.enabled = on;
        g_info.status = on ? Status::Working : Status::Off;
        g_info.errorKey.clear();
    }
    g_wake.notify_all();
}

namespace {
void Queue(Action action) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.actionBusy) return;
        g_info.actionBusy = true;
        g_info.actionKey.clear();
        g_action = action;
    }
    g_wake.notify_all();
}
}  // namespace

void ExportData() { Queue(Action::Export); }
void DeleteCloudCopy() { Queue(Action::DeleteCloud); }
void DeleteAccount() { Queue(Action::DeleteAccount); }

bool TakeLauncherReload() {
    return g_launcherReload.exchange(false);
}

}  // namespace luma::launcher::cloudsync
