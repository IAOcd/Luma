// =============================================================================
//  process_opts.h - Optimizaciones del proceso (Fase 4)
// =============================================================================
//  Cada una es un módulo separado (categoría Rendimiento), así se pueden
//  activar de a una y MEDIR con el benchmark si de verdad mejoran algo. Todas
//  son pedidos normales y documentados a Windows, que solo afectan a
//  Minecraft mientras está abierto, y se deshacen al desactivarlas o al
//  descargar Luma. Ninguna toca configuraciones de Windows.
//
//    - Prioridad del proceso: SetPriorityClass. Windows reparte la CPU
//      primero a los procesos de más prioridad.
//    - Timer de 1 ms: timeBeginPeriod(1). El "reloj" con el que Windows
//      despierta a los hilos que duermen pasa de ~15,6 ms a 1 ms: los frames
//      pueden salir más parejos (sobre todo con límite de FPS o V-Sync).
//      Cuesta un poco más de batería.
//    - Sin modo eficiencia (EcoQoS): Windows 11 puede poner a un proceso en
//      "modo eficiencia" (núcleos lentos, menos frecuencia), sobre todo si
//      la ventana no está en primer plano. Esto se lo prohíbe a Minecraft.
//    - Prioridad del hilo de render: solo el hilo que dibuja los frames
//      (el que llama a Present) sube de prioridad, no todo el juego.
//
//  Lo que NO hacemos (a propósito): "limpiar la RAM", cambiar el registro,
//  prioridad "tiempo real" (puede congelar el PC) ni nada que no se pueda
//  medir y deshacer.
// =============================================================================
#pragma once

#include "modules/module.h"

namespace luma::modules::performance {

class ProcessPriority : public Module {
public:
    ProcessPriority();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    void Apply() const;
    int level_ = 0;  // 0 = por encima de lo normal, 1 = alta
    unsigned long previous_ = 0;
};

// V-Sync off: FPS sin el límite del monitor (ver render/dx12_hooks.cpp).
class VSyncOff : public Module {
public:
    VSyncOff();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
};

class TimerResolution : public Module {
public:
    TimerResolution();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
};

class NoEcoQos : public Module {
public:
    NoEcoQos();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
};

class RenderThreadPriority : public Module {
public:
    RenderThreadPriority();
    const char* IconSvg() const override;
    void OnEnable() override;
    void OnDisable() override;
    bool HasSettings() const override { return true; }
    bool DrawSettings() override;
    void SaveSettings(nlohmann::json& out) const override;
    void LoadSettings(const nlohmann::json& in) override;

private:
    int level_ = 0;  // 0 = por encima de lo normal, 1 = la más alta
};

// La prioridad del hilo de render se aplica desde el hilo principal de Luma
// (cuando ya se sabe cuál es ese hilo). Se llama cada 50 ms.
void PollRenderThread();

}  // namespace luma::modules::performance
