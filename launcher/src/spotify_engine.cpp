// =============================================================================
//  spotify_engine.cpp - Luma's own Spotify player (see spotify_engine.h)
// =============================================================================
#include "spotify_engine.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <atomic>
#include <map>
#include <memory>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/paths.h"
#include "core/spotify.h"

namespace luma::launcher::spotifyengine {

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
State g_state = State::Off;
bool g_known = false;
std::string g_deviceId, g_error;
HANDLE g_job = nullptr;      // the player lives in this "job": closing it closes the player
HANDLE g_process = nullptr;
HANDLE g_input = nullptr;
// Questions to the player ("playlist <id>", "radio <uri>", "artist_top <id>"):
// question -> its answer and state (g_mutex)
std::map<std::string, std::shared_ptr<const nlohmann::json>> g_answers;
std::map<std::string, int> g_answerState;   // 0 asked, 1 here, 2 failed
std::map<std::string, std::string> g_requests;  // request number -> question
int g_nextRequest = 1;
std::vector<std::string> g_pendingLines;      // commands waiting for the player to be up (g_mutex)
bool g_up = false;                            // the player said "status: ready" (g_mutex)    // the player's input: the launcher's direct commands ("load ...")
std::thread g_reader;        // reads what the player says
std::atomic<double> g_startedAt{0.0};  // (the 60 s wait counts from "connecting")

double Now() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

// The program, next to the launcher.
fs::path ExePath() {
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return fs::path(std::wstring(self, n)).parent_path() / L"luma-spotify.exe";
}

fs::path CacheDir() {
    return paths::DataDir() / L"spotify-player";
}

// THE VOLUME: Windows' own volume of the player's sound (like the volume
// mixer: it changes at once). librespot's own volume changes the sound that is
// still to come, which is almost a second late, so it stays at 100%.
// Remembered (0..100) for the next start.
int LoadVolume() {
    FILE* f = nullptr;
    int v = 100;
    if (_wfopen_s(&f, (CacheDir() / L"volume.txt").c_str(), L"r") == 0 && f != nullptr) {
        if (std::fscanf(f, "%d", &v) != 1) v = 100;
        std::fclose(f);
    }
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

int g_volume = -1;                     // what you chose (0..100)
bool g_volumeDirty = false;            // (saved to the disk a moment after you stop)
double g_volumeChangedAt = 0.0;
bool g_volumeApplied = false;
ISimpleAudioVolume* g_session = nullptr;  // the player's sound in Windows' mixer
double g_sessionLookedAt = 0.0;

// The player's sound in Windows' mixer (it exists once it plays something).
ISimpleAudioVolume* FindSession(DWORD pid) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* manager = nullptr;
    IAudioSessionEnumerator* sessions = nullptr;
    ISimpleAudioVolume* found = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) &&
        SUCCEEDED(manager->GetSessionEnumerator(&sessions))) {
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count && found == nullptr; ++i) {
            IAudioSessionControl* control = nullptr;
            if (FAILED(sessions->GetSession(i, &control))) continue;
            IAudioSessionControl2* control2 = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2)))) {
                DWORD sessionPid = 0;
                if (SUCCEEDED(control2->GetProcessId(&sessionPid)) && sessionPid == pid) {
                    control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&found));
                }
                control2->Release();
            }
            control->Release();
        }
    }
    if (sessions != nullptr) sessions->Release();
    if (manager != nullptr) manager->Release();
    if (device != nullptr) device->Release();
    if (enumerator != nullptr) enumerator->Release();
    return found;
}

void ApplyVolume() {
    if (g_session == nullptr || g_volume < 0) return;
    // (straight, 0..1: the same scale the game's music volume uses, so both
    // bars show the same value and move each other)
    g_session->SetMasterVolume(static_cast<float>(g_volume) / 100.0f, nullptr);
    g_volumeApplied = true;
}

void ReleaseSession() {
    if (g_session != nullptr) {
        g_session->Release();
        g_session = nullptr;
    }
    g_volumeApplied = false;
}

// What happens, in %APPDATA%\Luma\logs\spotify_player.txt.
void Note(const std::string& text) {
    std::error_code ec;
    fs::create_directories(paths::DataDir() / L"logs", ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (paths::DataDir() / L"logs" / L"spotify_player.txt").c_str(), L"a") == 0 && f != nullptr) {
        std::fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, text.c_str());
        std::fclose(f);
    }
}

void Set(State s, const std::string& error = {}) {
    std::lock_guard lock(g_mutex);
    g_state = s;
    if (!error.empty()) g_error = error;
}

// Reads the player's lines ("status: ready", "error: ...") until it ends.
void Read(HANDLE pipe) {
    std::string pending;
    char buffer[512];
    DWORD got = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        pending.append(buffer, got);
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            if (line.rfind("answer ", 0) == 0) {
                // "answer <request> {json}"
                const std::size_t sp = line.find(' ', 7);
                if (sp != std::string::npos) {
                    const std::string req = line.substr(7, sp - 7);
                    auto j = std::make_shared<nlohmann::json>(nlohmann::json::parse(line.substr(sp + 1), nullptr, false));
                    const bool ok = j->is_object() && !j->contains("error");
                    std::lock_guard lock(g_mutex);
                    const std::string question = g_requests[req];
                    const std::string why = j->is_object() ? j->value("error", std::string()) : std::string();
                    Note(question + (ok ? std::string(": answered") : ": failed " + why));
                    g_answerState[question] = ok ? 1 : 2;
                    if (ok) g_answers[question] = std::move(j);
                }
                continue;
            }
            Note(line);
            if (line == "status: login") {
                Set(State::Login);
            } else if (line == "status: connecting") {
                g_startedAt.store(Now());
                Set(State::Starting);
            } else if (line == "status: ready") {
                std::vector<std::string> pending;
                {
                    std::lock_guard lock(g_mutex);
                    g_up = true;
                    pending.swap(g_pendingLines);
                }
                for (const std::string& l : pending) {
                    if (g_input != nullptr) {
                        const std::string text = l + "\n";
                        DWORD written = 0;
                        WriteFile(g_input, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                    }
                }
                spotify::RequestDevices();  // (Ready when Spotify lists it: GetState)
            } else if (line.rfind("error: ", 0) == 0) {
                Set(State::Error, line.substr(7));
            }
        }
    }
    CloseHandle(pipe);
}

}  // namespace

bool HighQuality() {
    std::error_code ec;
    return !fs::exists(CacheDir() / L"quality-normal", ec);  // (high, 320 kbps, unless chosen otherwise)
}

void SetHighQuality(bool high) {
    std::error_code ec;
    if (high) {
        fs::remove(CacheDir() / L"quality-normal", ec);
    } else {
        fs::create_directories(CacheDir(), ec);
        FILE* f = nullptr;
        if (_wfopen_s(&f, (CacheDir() / L"quality-normal").c_str(), L"w") == 0 && f != nullptr) std::fclose(f);
    }
    // (running: it restarts with the new quality)
    if (g_process != nullptr) {
        Stop();
        Start();
    }
}

void Start() {
    const State s = GetState();
    if (s == State::Starting || s == State::Ready || s == State::Login || s == State::NoProgram) return;
    Stop();
    std::error_code ec;
    fs::create_directories(CacheDir(), ec);
    if (g_job == nullptr) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (g_job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    // its output comes to us through a pipe
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        Set(State::Error, "no pipe");
        return;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        Set(State::Error, "no pipe");
        return;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + ExePath().wstring() + L"\" --name \"Luma\" --cache \"" + CacheDir().wstring() + L"\" --quality " + (HighQuality() ? std::wstring(L"320") : std::wstring(L"160")) + L" --volume 100";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = inRead;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   CacheDir().wstring().c_str(), &si, &pi);
    CloseHandle(writeEnd);
    CloseHandle(inRead);
    if (!ok) {
        CloseHandle(inWrite);
        CloseHandle(readEnd);
        Set(State::Error, "the player didn't open");
        return;
    }
    if (g_job != nullptr) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    g_input = inWrite;
    g_startedAt.store(Now());
    {
        std::lock_guard lock(g_mutex);
        g_deviceId.clear();
        g_error.clear();
    }
    Set(State::Starting);
    Note("player started (hidden)");
    g_reader = std::thread(Read, readEnd);
}

void Stop() {
    {
        std::lock_guard lock(g_mutex);
        g_up = false;
    }
    ReleaseSession();
    if (g_input != nullptr) {
        CloseHandle(g_input);
        g_input = nullptr;
    }
    if (g_process != nullptr) {
        TerminateProcess(g_process, 0);
        WaitForSingleObject(g_process, 2000);
        CloseHandle(g_process);
        g_process = nullptr;
    }
    if (g_reader.joinable()) g_reader.join();  // (the pipe closed with the player)
    std::lock_guard lock(g_mutex);
    if (g_state != State::NoProgram) g_state = State::Off;
    g_deviceId.clear();
}

void SignOut() {
    Stop();
    std::error_code ec;
    fs::remove(CacheDir() / L"credentials.json", ec);
    Note("signed out (the pass was deleted)");
}

State GetState() {
    if (g_volume < 0) g_volume = LoadVolume();
    if (g_process != nullptr && g_session == nullptr && Now() - g_sessionLookedAt > 1.0) {
        g_sessionLookedAt = Now();
        g_session = FindSession(GetProcessId(g_process));
        ApplyVolume();
    }
    if (g_volumeDirty && Now() - g_volumeChangedAt > 1.0) {
        g_volumeDirty = false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, (CacheDir() / L"volume.txt").c_str(), L"w") == 0 && f != nullptr) {
            std::fprintf(f, "%d", g_volume);
            std::fclose(f);
        }
    }
    {
        std::lock_guard lock(g_mutex);
        if (!g_known) {
            g_known = true;
            std::error_code ec;
            g_state = fs::exists(ExePath(), ec) ? State::Off : State::NoProgram;
        }
    }
    State s;
    {
        std::lock_guard lock(g_mutex);
        s = g_state;
    }
    if (g_process != nullptr && WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(g_process, &code);
        CloseHandle(g_process);
        g_process = nullptr;
        if (g_reader.joinable()) g_reader.join();
        Note("player stopped (" + std::to_string(code) + ")");
        std::lock_guard lock(g_mutex);
        if (g_state != State::Error) {
            g_state = State::Error;
            g_error = "the player stopped";
        }
        g_deviceId.clear();
        return g_state;
    }
    if ((s == State::Starting || s == State::Ready) && g_process != nullptr) {
        // is the "Luma" device in your Spotify yet?
        static std::vector<spotify::Device> list;
        static std::uint32_t version = 0;
        static double asked = 0.0;
        std::uint32_t v = version;
        if (spotify::Devices(version, list, v)) version = v;
        std::string id;
        for (const auto& d : list) {
            if (d.name == kDeviceName) id = d.id;
        }
        if (!id.empty()) {
            std::lock_guard lock(g_mutex);
            if (g_deviceId != id) Note("ready: the Luma device is in Spotify");
            g_deviceId = id;
            g_state = State::Ready;
            s = State::Ready;
        } else if (Now() - asked > 1.5) {
            asked = Now();
            spotify::RequestDevices();
            if (s == State::Ready) {
                std::lock_guard lock(g_mutex);
                g_state = State::Starting;  // (it disappeared: wait for it again)
            }
            if (s == State::Starting && Now() - g_startedAt.load() > 60.0) {
                Set(State::Error, "the Luma device didn't appear");
                s = State::Error;
            }
        }
    }
    return s;
}

void Command(const std::string& line) {
    if (g_input == nullptr) return;
    const std::string text = line + "\n";
    DWORD written = 0;
    WriteFile(g_input, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    if (line.rfind("volume", 0) != 0) Note("> " + line.substr(0, 120));
}

void SetVolume(int percent) {
    g_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    g_volumeDirty = true;
    g_volumeChangedAt = Now();
    ApplyVolume();
}

int Volume() {
    if (g_volume < 0) g_volume = LoadVolume();
    // the real one, from Windows: the game (or Windows' own mixer) may have moved it
    if (g_session != nullptr && Now() - g_volumeChangedAt > 0.5) {
        float v = 0.0f;
        if (SUCCEEDED(g_session->GetMasterVolume(&v))) {
            const int real = static_cast<int>(v * 100.0f + 0.5f);
            if (real != g_volume) {
                g_volume = real;
                g_volumeDirty = true;
                g_volumeChangedAt = Now() - 0.6;  // (saved a moment later, like any change)
            }
        }
    }
    return g_volume;
}

int Ask(const std::string& question, std::shared_ptr<const nlohmann::json>& out, bool again) {
    std::string line;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_answerState.find(question);
        if (it != g_answerState.end() && !(again && it->second != 0)) {
            if (it->second == 1) out = g_answers[question];
            return it->second;
        }
        g_answerState[question] = 0;
        const std::string req = std::to_string(g_nextRequest++);
        g_requests[req] = question;
        line = "ask " + req + " " + question;
    }
    bool up = false;
    {
        std::lock_guard lock(g_mutex);
        up = g_up;
        if (!up) g_pendingLines.push_back(line);
    }
    if (up) {
        Command(line);
    } else {
        const State st = GetState();
        if (st == State::Off || st == State::Error) Start();  // (it only connects: nothing plays)
    }
    return 0;
}

int PlaylistSongs(const std::string& playlistId, std::shared_ptr<const nlohmann::json>& out) {
    return Ask("playlist " + playlistId, out, false);
}

std::string DeviceId() {
    std::lock_guard lock(g_mutex);
    return g_deviceId;
}

std::string Error() {
    std::lock_guard lock(g_mutex);
    return g_error;
}

}  // namespace luma::launcher::spotifyengine
