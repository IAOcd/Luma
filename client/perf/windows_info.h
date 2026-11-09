// =============================================================================
//  windows_info.h - Leer (SOLO leer) configuraciones de Windows útiles para jugar
// =============================================================================
//  Para la sección de ayuda del menú: Luma te muestra cómo están estas
//  opciones y cómo cambiarlas, pero NUNCA las cambia por su cuenta.
//
//  Todas se leen del registro de Windows o con funciones oficiales:
//    - Modo de juego:   HKCU\Software\Microsoft\GameBar, AutoGameModeEnabled
//    - Programación de GPU acelerada por hardware ("HAGS"):
//                       HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers, HwSchMode (2 = sí)
//    - Plan de energía: PowerGetActiveScheme + su nombre
//    - GPU preferida para Minecraft:
//                       HKCU\Software\Microsoft\DirectX\UserGpuPreferences, valor = ruta del .exe
//    - Optimizaciones para juegos en ventana: el mismo lugar, "DirectXUserGlobalSettings"
// =============================================================================
#pragma once

#include <string>

namespace luma::perf::windows {

enum class Status { On, Off, Unknown };

struct Info {
    Status gameMode = Status::Unknown;
    Status gpuScheduling = Status::Unknown;
    Status windowedOptimizations = Status::Unknown;
    std::string powerPlan;          // nombre del plan activo ("Equilibrado"...)
    bool powerPlanIsHighPerformance = false;
    std::string gpuPreference;      // "Alto rendimiento", "Ahorro de energía", "Lo decide Windows"
};

// Lee todo (unos pocos milisegundos: llamarlo al abrir la página, no por frame).
Info Read();

}  // namespace luma::perf::windows
