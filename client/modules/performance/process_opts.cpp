// =============================================================================
//  process_opts.cpp - Implementación de las optimizaciones del proceso
// =============================================================================
#include "modules/performance/process_opts.h"

#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod (winmm)

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "core/logger.h"
#include "perf/frame_stats.h"
#include "render/icons.h"
#include "render/ui.h"

// Esta bandera es de Windows 11 (SDK 22000+). Si el SDK es más viejo, la
// definimos con su valor oficial.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace luma::modules::performance {

namespace {

// Un cronómetro (con el botón arriba): distinto del Reloj del HUD.

// --- Ahorro de energía del proceso ("power throttling") ------------------------
// Dos de las optimizaciones usan la misma llamada de Windows, cada una con su
// bandera. Guardamos cuáles están pedidas y mandamos las dos juntas.
std::atomic<ULONG> g_throttlingControl{0};

void ApplyPowerThrottling() {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // ControlMask = qué mecanismos controlamos nosotros; StateMask = 0 =
    // "apagados". Lo que no está en ControlMask lo decide Windows como siempre.
    state.ControlMask = g_throttlingControl.load();
    state.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
        log::Warn("No se pudo cambiar el ahorro de energía del proceso (error {}).", GetLastError());
    }
}

void SetThrottlingFlag(ULONG flag, bool on) {
    if (on) {
        g_throttlingControl.fetch_or(flag);
    } else {
        g_throttlingControl.fetch_and(~flag);
    }
    ApplyPowerThrottling();
}

// --- Hilo de render ------------------------------------------------------------
std::mutex g_threadMutex;
std::atomic<int> g_threadDesired{-1};  // -1 = sin cambio; 0/1 = nivel pedido
int g_threadApplied = -1;
int g_threadOriginal = THREAD_PRIORITY_NORMAL;

}  // namespace

// =============================================================================
//  Prioridad del proceso
// =============================================================================

ProcessPriority::ProcessPriority()
    : Module("opt_priority", "Process priority",
             "Windows le da a Minecraft la CPU antes que a otros programas. \"Por encima de lo normal\" es "
             "seguro; \"Alta\" puede hacer que otros programas (Discord, el navegador) vayan lentos.",
             Category::Performance) {}

const char* ProcessPriority::IconSvg() const {
    return render::icons::kZap;
}

void ProcessPriority::Apply() const {
    const DWORD wanted = level_ == 1 ? HIGH_PRIORITY_CLASS : ABOVE_NORMAL_PRIORITY_CLASS;
    if (SetPriorityClass(GetCurrentProcess(), wanted)) {
        log::Info("Prioridad del proceso: {}.", level_ == 1 ? "alta" : "por encima de lo normal");
    } else {
        log::Warn("No se pudo cambiar la prioridad del proceso (error {}).", GetLastError());
    }
}

void ProcessPriority::OnEnable() {
    previous_ = GetPriorityClass(GetCurrentProcess());
    Apply();
}

void ProcessPriority::OnDisable() {
    SetPriorityClass(GetCurrentProcess(), previous_ != 0 ? previous_ : NORMAL_PRIORITY_CLASS);
    log::Info("Prioridad del proceso restaurada.");
}

bool ProcessPriority::DrawSettings() {
    const bool changed = render::ui::Choice("Level", &level_, {"Above normal", "High"});
    if (changed && IsEnabled()) {
        Apply();
    }
    return changed;
}

void ProcessPriority::SaveSettings(nlohmann::json& out) const {
    out["level"] = level_;
}

void ProcessPriority::LoadSettings(const nlohmann::json& in) {
    level_ = std::clamp(in.value("level", 0), 0, 1);
}

// =============================================================================
//  Timer de 1 ms
// =============================================================================

TimerResolution::TimerResolution()
    : Module("opt_timer", "1 ms timer",
             "Sube la precisión del reloj de Windows a 1 ms para Minecraft: los frames pueden salir más parejos "
             "(sobre todo con límite de FPS). Usa un poco más de batería.",
             Category::Performance) {}

const char* TimerResolution::IconSvg() const {
    return render::icons::kTimer;
}

void TimerResolution::OnEnable() {
    timeBeginPeriod(1);
    // Windows 11 ignora este pedido si la ventana está minimizada o tapada;
    // así le pedimos que lo respete siempre.
    SetThrottlingFlag(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, true);
    log::Info("Timer de 1 ms activado.");
}

void TimerResolution::OnDisable() {
    timeEndPeriod(1);  // siempre emparejado con el timeBeginPeriod de arriba
    SetThrottlingFlag(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, false);
    log::Info("Timer de 1 ms desactivado.");
}

// =============================================================================
//  Sin modo eficiencia (EcoQoS)
// =============================================================================

NoEcoQos::NoEcoQos()
    : Module("opt_ecoqos", "No efficiency mode",
             "Impide que Windows 11 ponga a Minecraft en \"modo eficiencia\" (EcoQoS: núcleos lentos y menos "
             "frecuencia), algo que hace sobre todo con ventanas en segundo plano.",
             Category::Performance) {}

const char* NoEcoQos::IconSvg() const {
    return render::icons::kLeaf;
}

void NoEcoQos::OnEnable() {
    SetThrottlingFlag(PROCESS_POWER_THROTTLING_EXECUTION_SPEED, true);
    log::Info("Modo eficiencia (EcoQoS) desactivado para Minecraft.");
}

void NoEcoQos::OnDisable() {
    SetThrottlingFlag(PROCESS_POWER_THROTTLING_EXECUTION_SPEED, false);
    log::Info("Modo eficiencia: vuelve a decidir Windows.");
}

// =============================================================================
//  Prioridad del hilo de render
// =============================================================================

RenderThreadPriority::RenderThreadPriority()
    : Module("opt_render_thread", "Render thread priority",
             "Raises the priority of only the thread that draws the frames (not the whole game).",
             Category::Performance) {}

const char* RenderThreadPriority::IconSvg() const {
    return render::icons::kCpu;
}

void RenderThreadPriority::OnEnable() {
    g_threadDesired.store(level_);
    PollRenderThread();
}

void RenderThreadPriority::OnDisable() {
    g_threadDesired.store(-1);
    PollRenderThread();
}

bool RenderThreadPriority::DrawSettings() {
    const bool changed = render::ui::Choice("Level", &level_, {"Above normal", "Highest"});
    if (changed && IsEnabled()) {
        g_threadDesired.store(level_);
        PollRenderThread();
    }
    if (perf::RenderThreadId() == 0) {
        render::ui::Hint("The render thread hasn't been seen yet.");
    }
    return changed;
}

void RenderThreadPriority::SaveSettings(nlohmann::json& out) const {
    out["level"] = level_;
}

void RenderThreadPriority::LoadSettings(const nlohmann::json& in) {
    level_ = std::clamp(in.value("level", 0), 0, 1);
}

void PollRenderThread() {
    std::lock_guard lock(g_threadMutex);
    const int desired = g_threadDesired.load();
    if (desired == g_threadApplied) {
        return;
    }
    const DWORD id = perf::RenderThreadId();
    if (id == 0) {
        return;  // todavía no hubo un frame: se aplica en la próxima vuelta
    }
    HANDLE thread = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (thread == nullptr) {
        log::Warn("No se pudo abrir el hilo de render (error {}).", GetLastError());
        g_threadApplied = desired;  // no reintentar sin parar
        return;
    }
    if (g_threadApplied == -1) {
        g_threadOriginal = GetThreadPriority(thread);  // la de antes, para devolverla
    }
    const int priority = desired == -1  ? g_threadOriginal
                         : desired == 1 ? THREAD_PRIORITY_HIGHEST
                                        : THREAD_PRIORITY_ABOVE_NORMAL;
    if (SetThreadPriority(thread, priority)) {
        log::Info("Hilo de render (id {}): prioridad {}.", id, desired == -1 ? "original" : (desired == 1 ? "la más alta" : "por encima de lo normal"));
    }
    CloseHandle(thread);
    g_threadApplied = desired;
}

}  // namespace luma::modules::performance
