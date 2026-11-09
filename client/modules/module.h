// =============================================================================
//  module.h - La interfaz común de todos los módulos
// =============================================================================
//  Un módulo es una función de Luma que se puede activar y desactivar: el
//  contador de FPS, el zoom, el minimapa, etc. Todos comparten esta "forma"
//  (clase base). Cada módulo concreto hereda de Module y completa solo lo que
//  necesita:
//
//      class MiModulo : public Module {
//      public:
//          MiModulo() : Module("mi_modulo", "Mi módulo", "Qué hace", Category::Hud) {}
//          bool HasHud() const override { return true; }
//          void OnRenderHud() override { ...dibujar... }
//      };
//
//  "virtual" significa que cada módulo puede reemplazar ("override") esa
//  función con su propia versión. Las que no reemplaza usan la de aquí, que
//  no hace nada.
//
//  RENDIMIENTO: el gestor de módulos solo llama a OnRenderHud de los módulos
//  ACTIVOS. Un módulo desactivado no ejecuta ni una instrucción por frame.
//
//  HILOS: todas estas funciones se llaman con luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include "core/keys.h"

namespace luma::modules {

enum class Category {
    Hud,          // Elementos en pantalla: FPS, coordenadas, keystrokes...
    Visual,       // Zoom, fullbright, FOV, crosshair...
    Utility,      // Toggle sprint, chat, avisos...
    World,        // Minimapa, waypoints, free cam...
    Performance,  // Overlay de rendimiento, optimizaciones...
    Count         // (no es una categoría: es la cantidad)
};

const char* CategoryName(Category category);

class Module {
public:
    Module(std::string id, std::string name, std::string description, Category category);
    virtual ~Module() = default;

    // No se copian: cada módulo existe una sola vez.
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    // "id" es el nombre interno que se usa en la config (sin espacios ni
    // tildes, y nunca cambia). "name" es el que se muestra en el menú.
    const std::string& Id() const { return id_; }
    const std::string& Name() const { return name_; }
    const std::string& Description() const { return description_; }
    Category GetCategory() const { return category_; }

    bool IsEnabled() const { return enabled_; }
    int Keybind() const { return keybind_; }
    void SetKeybind(int virtualKey) { keybind_ = virtualKey; }

    // ¿Solo debe funcionar en mundos locales? (Se aplica desde la Fase 7,
    // cuando Luma sepa distinguir mundo local de servidor.)
    bool IsLocalOnly() const { return localOnly_; }

    // --- Lo que cada módulo puede reemplazar ----------------------------------

    // Su ícono: un path SVG en una cuadrícula de 24x24 (ver render/icons.h).
    // nullptr = el ícono de su categoría.
    virtual const char* IconSvg() const { return nullptr; }

    // ¿Dibuja algo en pantalla cada frame mientras está activo?
    virtual bool HasHud() const { return false; }
    // Dibuja su parte del HUD (se llama dentro de un frame de ImGui).
    virtual void OnRenderHud() {}

    // Al activarse / desactivarse.
    virtual void OnEnable() {}
    virtual void OnDisable() {}

    // ¿Tiene opciones propias? Si sí, el menú muestra un desplegable que
    // llama a DrawSettings.
    virtual bool HasSettings() const { return false; }
    // Dibuja sus opciones en el menú. Devuelve true si alguna cambió (para
    // guardar la config).
    virtual bool DrawSettings() { return false; }

    // Guardar / cargar SUS opciones (enabled y keybind los guarda el gestor).
    virtual void SaveSettings(nlohmann::json& out) const { (void)out; }
    virtual void LoadSettings(const nlohmann::json& in) { (void)in; }

private:
    // Solo el gestor activa/desactiva (así siempre mantiene su lista al día).
    friend class Manager;
    void SetEnabledInternal(bool enabled) { enabled_ = enabled; }

protected:
    bool localOnly_ = false;

private:
    std::string id_;
    std::string name_;
    std::string description_;
    Category category_;
    bool enabled_ = false;
    int keybind_ = keys::kNone;
};

}  // namespace luma::modules
