// =============================================================================
//  menu.cpp - Menú de Luma (Fase 2a: menú vacío)
// =============================================================================
//  ImGui es una biblioteca "immediate mode": no se crean botones que viven
//  para siempre, sino que CADA FRAME se describe la interfaz desde cero:
//
//      if (ImGui::Button("Hola")) { ...esto corre el frame en que se pulsa... }
//
//  ImGui se encarga de recordar posiciones, tamaños y estados internamente.
// =============================================================================
#include "render/menu.h"

#include <algorithm>
#include <array>
#include <cstddef>

#include <imgui.h>

#include "core/version.h"
#include "perf/frame_stats.h"

namespace luma::render::menu {

namespace {

// Color de acento de Luma: ámbar cálido. (R, G, B, A) de 0 a 1.
// Será configurable en la Fase 3.
constexpr ImVec4 kAccent{1.00f, 0.72f, 0.30f, 1.00f};

// Mezcla el acento con transparencia, para variantes más suaves.
ImVec4 Accent(float alpha) {
    return ImVec4(kAccent.x, kAccent.y, kAccent.z, alpha);
}

}  // namespace

void ApplyStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);  // Base oscura...

    // ...con esquinas redondeadas y algo más de aire.
    style.WindowRounding = 10.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.WindowBorderSize = 1.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_Border] = Accent(0.25f);
    c[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.09f, 0.11f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.11f, 0.10f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = Accent(0.20f);
    c[ImGuiCol_FrameBgActive] = Accent(0.35f);
    c[ImGuiCol_Button] = Accent(0.22f);
    c[ImGuiCol_ButtonHovered] = Accent(0.45f);
    c[ImGuiCol_ButtonActive] = Accent(0.65f);
    c[ImGuiCol_Header] = Accent(0.22f);
    c[ImGuiCol_HeaderHovered] = Accent(0.40f);
    c[ImGuiCol_HeaderActive] = Accent(0.55f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Separator] = Accent(0.25f);
    c[ImGuiCol_Tab] = Accent(0.15f);
    c[ImGuiCol_TabHovered] = Accent(0.45f);
    c[ImGuiCol_TabSelected] = Accent(0.35f);
    c[ImGuiCol_PlotLines] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;

    // Agrandar todos los tamaños (márgenes, bordes, etc.) según la resolución.
    style.ScaleAllSizes(scale);
}

namespace {

void DrawGeneralTab(const RenderInfo& info) {
    ImGui::TextWrapped("El menú está vacío por ahora: los módulos llegan en la Fase 3.");
    ImGui::Spacing();
    ImGui::BulletText("L: abrir / cerrar este menú");
    ImGui::BulletText("Fin (End): descargar Luma");
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Información de render")) {
        ImGui::Text("API: DirectX 12");
        ImGui::Text("Resolución: %u x %u", info.width, info.height);
        ImGui::Text("Formato: %s", info.formatName);
        ImGui::Text("Buffers de la swapchain: %u", info.bufferCount);
    }
}

// Estadísticas calculadas a partir del historial de frames.
struct HistoryStats {
    float averageMs = 0.0f;
    float maxMs = 0.0f;
    float percentile99Ms = 0.0f;  // el frametime que solo supera el 1% más lento
    float openCostAvgUs = 0.0f;   // costo promedio con el menú abierto
    float openCostMaxUs = 0.0f;
};

HistoryStats ComputeStats(const perf::History& history) {
    HistoryStats stats;
    if (history.count == 0) {
        return stats;
    }
    // Copia en un arreglo de tamaño fijo (en la pila, sin pedir memoria) para
    // poder ordenarla sin desordenar el historial.
    std::array<float, perf::kHistorySize> sorted{};
    double sum = 0.0;
    double costSum = 0.0;
    std::size_t costCount = 0;
    for (std::size_t i = 0; i < history.count; ++i) {
        const float ms = history.frameMs[i];
        sorted[i] = ms;
        sum += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
        if (history.menuOpen[i]) {
            costSum += history.costUs[i];
            stats.openCostMaxUs = std::max(stats.openCostMaxUs, history.costUs[i]);
            ++costCount;
        }
    }
    stats.averageMs = static_cast<float>(sum / static_cast<double>(history.count));
    if (costCount > 0) {
        stats.openCostAvgUs = static_cast<float>(costSum / static_cast<double>(costCount));
    }
    // nth_element deja en la posición pedida el valor que estaría ahí si el
    // arreglo estuviera ordenado, sin ordenarlo entero (más rápido).
    const std::size_t index = (history.count * 99) / 100;
    auto end = sorted.begin() + static_cast<std::ptrdiff_t>(history.count);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), end);
    stats.percentile99Ms = sorted[index];
    return stats;
}

void DrawPerformanceTab() {
    const perf::History& history = perf::GetHistory();
    const HistoryStats stats = ComputeStats(history);
    const float fps = stats.averageMs > 0.0f ? 1000.0f / stats.averageMs : 0.0f;
    const float low1 = stats.percentile99Ms > 0.0f ? 1000.0f / stats.percentile99Ms : 0.0f;

    // --- Gráfico de frametime -------------------------------------------------
    ImGui::Text("FPS: %.0f   1%% low: %.0f   Frametime: %.2f ms (máx %.2f ms)", fps, low1,
                stats.averageMs, stats.maxMs);

    // Escala del gráfico: al menos 33 ms (30 FPS), o un poco más que el máximo.
    const float scaleMax = std::max(33.3f, stats.maxMs * 1.1f);
    // Si el historial ya dio la vuelta, "next" es el dato más viejo: el
    // gráfico empieza desde ahí para dibujarse en orden.
    const int offset = history.count == perf::kHistorySize ? static_cast<int>(history.next) : 0;
    ImGui::PlotLines("##frametime", history.frameMs.data(), static_cast<int>(history.count), offset,
                     nullptr, 0.0f, scaleMax,
                     ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 6.0f));
    ImGui::TextDisabled("Últimos %zu frames. Más abajo = mejor; picos = tirones.", history.count);

    ImGui::Spacing();
    ImGui::SeparatorText("Costo de Luma");

    // --- Costo con el menú cerrado (lo importante) ------------------------------
    const perf::Summary summary = perf::LastSummary();
    if (summary.valid && summary.closedFrames > 0) {
        // Criterio de la Fase 2: menos de 0,1 ms (100 µs) por frame.
        const bool ok = summary.averageCostUs < 100.0;
        ImGui::Text("Menú cerrado (últimos %.0f s):", summary.seconds);
        ImGui::SameLine();
        ImGui::TextColored(ok ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                           "%.4f ms promedio", summary.averageCostUs / 1000.0);
        ImGui::Text("   máximo %.4f ms  ·  %llu frames", summary.maxCostUs / 1000.0,
                    static_cast<unsigned long long>(summary.closedFrames));
    } else {
        ImGui::TextWrapped("Menú cerrado: todavía no hay datos. Deja el menú cerrado 30 s y "
                           "vuelve a abrirlo (el resumen también se escribe en el log).");
    }

    // --- Costo con el menú abierto -----------------------------------------------
    ImGui::Text("Menú abierto (ahora): %.3f ms promedio, %.3f ms máximo",
                stats.openCostAvgUs / 1000.0f, stats.openCostMaxUs / 1000.0f);
    ImGui::TextDisabled("Con el menú abierto el costo es mayor porque ImGui dibuja la ventana.");
}

}  // namespace

void Draw(const RenderInfo& info) {
    // Tamaño y posición iniciales (solo la primera vez; después el usuario
    // puede mover y redimensionar la ventana).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.36f, viewport->Size.y * 0.5f),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x * 0.5f, viewport->Size.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    // Begin/End delimitan una ventana. Todo lo que va entre ellos se dibuja
    // dentro. (End se llama SIEMPRE, aunque Begin devuelva false.)
    if (ImGui::Begin(kName, nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(kAccent, "%s %s", kName, kVersion);
        ImGui::Separator();

        // Pestañas: solo se dibuja el contenido de la pestaña elegida.
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("General")) {
                DrawGeneralTab(info);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Rendimiento")) {
                DrawPerformanceTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

}  // namespace luma::render::menu
