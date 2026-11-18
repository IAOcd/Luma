// =============================================================================
//  gallery.cpp - The Lyric Flow style gallery (see gallery.h)
// =============================================================================
#include "gallery.h"

#include "account.h"
#include "net.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace luma::launcher::gallery {

namespace {

using json = nlohmann::json;

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<std::function<void()>> g_jobs;
std::thread g_thread;
bool g_quit = false;
Info g_info;

// The keys of "how it looks" (the same list the server accepts).
constexpr const char* kLookKeys[] = {
    "opacity", "uppercase", "appear", "leave", "motion", "speed", "colorMode", "color", "color2", "highlight",
    "highlightColor", "outline", "outlineColor", "outlineAlpha", "shadow", "shadowStyle", "font", "bold",
    "resolution", "letterSpacing", "wordSpacing", "leaveTime", "fill", "fillWholeLine", "unsung", "unsungColor",
    "glow", "glowColor", "glowCustom", "glowStrength", "glowWhere", "vfx", "vfxAmount", "vfxColor",
};

void Worker() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock, [] { return g_quit || !g_jobs.empty(); });
            if (g_quit) return;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        job();
    }
}

void Queue(std::function<void()> job) {
    std::lock_guard lock(g_mutex);
    if (g_quit) return;
    if (!g_thread.joinable()) g_thread = std::thread(Worker);
    g_jobs.push_back(std::move(job));
    g_wake.notify_one();
}

struct Reply {
    int status = 0;  // 0 = no connection
    json body = json::object();
};

Reply Call(const wchar_t* method, const std::wstring& path, const std::string& body = {}, bool withPass = true) {
    Reply reply;
    std::wstring headers = L"Accept: application/json\r\n";
    const std::string pass = withPass ? account::LumaPass() : std::string();
    if (!pass.empty()) headers += L"Authorization: Bearer " + net::Widen(pass) + L"\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
    net::Response r;
    if (!net::Request(method, account::ApiBase() + path, body, headers, r, 2 * 1024 * 1024)) return reply;
    reply.status = r.status;
    reply.body = json::parse(r.body, nullptr, false);
    if (reply.body.is_discarded() || !reply.body.is_object()) reply.body = json::object();
    return reply;
}

// The server's answer -> a translation key ("gallery.err_...").
std::string ErrorKey(const Reply& r) {
    if (r.status == 0) return "gallery.err_offline";
    if (r.status == 401) return "gallery.err_session";
    const std::string code = r.body.value("error", std::string());
    if (code == "name_blocked" || code == "name_link" || code == "bad_name" || code == "duplicate" || code == "already_in_gallery" || code == "too_many" ||
        code == "too_many_today" || code == "banned" || code == "bad_look" || code == "not_found") {
        return "gallery.err_" + code;
    }
    return "gallery.err_server";
}

Style ParseStyle(const json& j) {
    Style s;
    if (!j.is_object()) return s;
    s.id = j.value("id", std::string());
    s.name = j.value("name", std::string());
    if (j.contains("author") && j["author"].is_string()) s.author = j["author"].get<std::string>();
    if (j.contains("look") && j["look"].is_object()) s.look = j["look"];
    s.installs = j.value("installs", 0);
    s.created = j.value("created", 0LL);
    s.status = j.value("status", std::string());
    return s;
}

void Done(const std::string& key, bool ok) {
    std::lock_guard lock(g_mutex);
    g_info.busy = false;
    g_info.actionKey = key;
    g_info.actionOk = ok;
    ++g_info.version;
}

void FetchMine() {
    const Reply r = Call(L"GET", L"/v1/me/gallery");
    std::lock_guard lock(g_mutex);
    g_info.mineLoading = false;
    if (r.status != 200) return;
    g_info.mineLoaded = true;
    g_info.banned = r.body.value("banned", false);
    g_info.mine.clear();
    if (r.body.contains("styles") && r.body["styles"].is_array()) {
        for (const json& s : r.body["styles"]) g_info.mine.push_back(ParseStyle(s));
    }
    ++g_info.version;
}

}  // namespace

json LookOnly(const json& look) {
    json out = json::object();
    if (!look.is_object()) return out;
    for (const char* key : kLookKeys) {
        const auto it = look.find(key);
        if (it != look.end()) out[key] = *it;
    }
    return out;
}

void Load(Sort sort, bool more) {
    int offset = 0;
    {
        std::lock_guard lock(g_mutex);
        if (g_info.loading) return;
        if (more) {
            if (!g_info.hasMore || g_info.sort != sort) return;
            offset = static_cast<int>(g_info.styles.size());
        }
        g_info.loading = true;
        g_info.sort = sort;
        g_info.errorKey.clear();
        ++g_info.version;
    }
    Queue([sort, more, offset] {
        const std::wstring path = std::wstring(L"/v1/gallery?sort=") + (sort == Sort::Popular ? L"popular" : L"new") +
                                  L"&cursor=" + std::to_wstring(offset);
        const Reply r = Call(L"GET", path, {}, false);
        std::lock_guard lock(g_mutex);
        g_info.loading = false;
        ++g_info.version;
        if (g_info.sort != sort) return;  // (you switched meanwhile)
        if (r.status != 200) {
            g_info.errorKey = ErrorKey(r);
            return;
        }
        if (!more) g_info.styles.clear();
        if (r.body.contains("styles") && r.body["styles"].is_array()) {
            for (const json& s : r.body["styles"]) {
                Style style = ParseStyle(s);
                if (!style.id.empty()) g_info.styles.push_back(std::move(style));
            }
        }
        g_info.hasMore = r.body.contains("next") && r.body["next"].is_number();
        g_info.loaded = true;
    });
}

void LoadMine() {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.mineLoading) return;
        g_info.mineLoading = true;
    }
    Queue(FetchMine);
}

void Publish(const std::string& name, const json& look, bool showAuthor) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.busy) return;
        g_info.busy = true;
        g_info.actionKey.clear();
    }
    const std::string body = json{{"name", name}, {"look", LookOnly(look)}, {"showAuthor", showAuthor}}.dump();
    Queue([body] {
        const Reply r = Call(L"POST", L"/v1/gallery", body);
        if (r.status == 200) {
            FetchMine();
            Done("gallery.published", true);
            std::lock_guard lock(g_mutex);
            g_info.loaded = false;  // (Explore loads again, with yours in it)
        } else {
            Done(ErrorKey(r), false);
        }
    });
}

void Unpublish(const std::string& id) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.busy) return;
        g_info.busy = true;
        g_info.actionKey.clear();
    }
    Queue([id] {
        const Reply r = Call(L"DELETE", L"/v1/gallery/" + net::Widen(id));
        if (r.status == 200) {
            {
                std::lock_guard lock(g_mutex);
                std::erase_if(g_info.styles, [&](const Style& s) { return s.id == id; });
            }
            FetchMine();
            Done("gallery.removed", true);
        } else {
            Done(ErrorKey(r), false);
        }
    });
}

void LoadLinked(const std::string& id) {
    {
        std::lock_guard lock(g_mutex);
        g_info.linkedId = id;
        g_info.linkedLoading = true;
        g_info.linkedErrorKey.clear();
        g_info.linked = Style{};
        ++g_info.version;
    }
    Queue([id] {
        const Reply r = Call(L"GET", L"/v1/gallery/" + net::Widen(id), {}, false);
        std::lock_guard lock(g_mutex);
        if (g_info.linkedId != id) return;  // (another link came meanwhile)
        g_info.linkedLoading = false;
        ++g_info.version;
        if (r.status == 200) {
            g_info.linked = ParseStyle(r.body);
        } else {
            g_info.linkedErrorKey = ErrorKey(r);
        }
    });
}

void ClearLinked() {
    std::lock_guard lock(g_mutex);
    g_info.linkedId.clear();
    g_info.linked = Style{};
    g_info.linkedErrorKey.clear();
    ++g_info.version;
}

void CountInstall(const std::string& id) {
    Queue([id] { Call(L"POST", L"/v1/gallery/" + net::Widen(id) + L"/install", "{}"); });
}

void Report(const std::string& id, const std::string& reason) {
    {
        std::lock_guard lock(g_mutex);
        if (g_info.busy) return;
        g_info.busy = true;
        g_info.actionKey.clear();
    }
    const std::string body = json{{"reason", reason}}.dump();
    Queue([id, body] {
        const Reply r = Call(L"POST", L"/v1/gallery/" + net::Widen(id) + L"/report", body);
        Done(r.status == 200 ? "gallery.reported" : ErrorKey(r), r.status == 200);
    });
}

void Reset() {
    std::lock_guard lock(g_mutex);
    g_info.mine.clear();
    g_info.mineLoaded = false;
    g_info.banned = false;
    g_info.actionKey.clear();
    ++g_info.version;
}

Info Get() {
    std::lock_guard lock(g_mutex);
    return g_info;
}

void Stop() {
    {
        std::lock_guard lock(g_mutex);
        g_quit = true;
        g_jobs.clear();
    }
    g_wake.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace luma::launcher::gallery
