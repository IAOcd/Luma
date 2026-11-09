// =============================================================================
//  module_manager.h - El gestor de módulos
// =============================================================================
//  Guarda la lista de todos los módulos, los activa/desactiva, reacciona a
//  sus teclas y le pide al HUD que dibuje solo los activos.
//
//  HILOS: todas las funciones (salvo HasHud) se llaman con
//  luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

#include "modules/module.h"

namespace luma::modules {

class Manager {
public:
    // Crea todos los módulos (ver registry.cpp). Una vez, al arrancar.
    static void RegisterAll();

    static const std::vector<std::unique_ptr<Module>>& All();
    static Module* Find(std::string_view id);

    // Activa o desactiva un módulo. "notify" = mostrar un aviso (toast).
    static void SetEnabled(Module& module, bool enabled, bool notify);
    static void Toggle(Module& module, bool notify) { SetEnabled(module, !module.IsEnabled(), notify); }

    // Una tecla fue pulsada (desde la ventana del juego). Activa/desactiva
    // los módulos que la tengan asignada. Devuelve true si alguno cambió.
    static bool HandleKey(int virtualKey);

    // Dibuja el HUD de los módulos activos (hilo de render, dentro de un frame
    // de ImGui).
    static void RenderHud();

    // ¿Hay algún módulo activo que dibuje en pantalla? Se puede llamar SIN el
    // candado: es una variable atómica. Es la clave del "camino rápido": si
    // es false (y no hay avisos ni menú), no se ejecuta ImGui.
    static bool HasHud() { return hasHud_.load(std::memory_order_relaxed); }

    // Config: guardar y cargar el estado de todos los módulos.
    static void Save(nlohmann::json& out);
    static void Load(const nlohmann::json& in);

    // Desactiva todo (al descargar Luma).
    static void Shutdown();

private:
    static void RebuildActiveLists();

    static std::vector<std::unique_ptr<Module>> modules_;
    // Solo los módulos activos con HUD. Se reconstruye al activar/desactivar
    // (nunca por frame) y tiene espacio reservado de antemano.
    static std::vector<Module*> activeHud_;
    static std::atomic<bool> hasHud_;
};

}  // namespace luma::modules
