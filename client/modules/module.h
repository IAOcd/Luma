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

#include <imgui.h>
#include "core/tr.h"
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include "core/keys.h"
#include "render/hud.h"

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

// Dónde va un elemento del HUD. Se guarda RELATIVO a la pantalla (fracciones
// de 0 a 1), así sirve en cualquier resolución.
//
// "Ancla": qué punto del elemento se ubica en (x, y). Un elemento pegado al
// borde derecho tiene ancla 1 en x: si su texto se alarga ("99 FPS" ->
// "144 FPS"), crece hacia la izquierda en vez de salirse de la pantalla. El
// editor elige el ancla solo según en qué tercio de la pantalla lo sueltes.
struct HudLayout {
    float x = 0.01f;       // posición del ancla, fracción del ancho de pantalla
    float y = 0.01f;       // ... del alto
    float anchorX = 0.0f;  // 0 = izquierda, 0.5 = centro, 1 = derecha del elemento
    float anchorY = 0.0f;  // 0 = arriba, 0.5 = centro, 1 = abajo
    float scale = 1.0f;
};

// Lo que recibe un módulo para dibujar su HUD.
struct HudFrame {
    ImDrawList* draw = nullptr;
    ImVec2 pos{0, 0};     // esquina superior izquierda donde dibujar
    float scale = 1.0f;   // escala elegida en el editor
    bool editing = false; // ¿está abierto el editor de HUD? (para mostrar un ejemplo si no hay datos)
};

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
    // El nombre y la descripción en el idioma elegido (ver core/tr.h).
    const char* DisplayName() const { return Tr(name_.c_str()); }
    const char* DisplayDescription() const { return Tr(description_.c_str()); }
    Category GetCategory() const { return category_; }

    bool IsEnabled() const { return enabled_; }
    int Keybind() const { return keybind_; }
    void SetKeybind(int virtualKey) { keybind_ = virtualKey; }

    // ¿Solo debe funcionar en mundos locales? (Se aplica desde la Fase 7,
    // cuando Luma sepa distinguir mundo local de servidor.)
    bool IsLocalOnly() const { return localOnly_; }

    // ¿Necesita leer al jugador del juego (sdk/player.h)? Si el SDK no lo
    // encuentra en esta versión del juego, el módulo queda "no disponible".
    virtual bool NeedsPlayer() const { return false; }
    // ¿Necesita el hook de red (ping)? (sdk/network.h)
    virtual bool NeedsNetwork() const { return false; }
    // ¿Se encontraron las firmas del juego que este módulo usa? (por ejemplo,
    // Fullbright necesita la de getGamma). false = "no disponible".
    virtual bool SdkReady() const { return true; }
    // ¿Su tecla se usa mucho (zoom, freelook)? Entonces no mostramos un aviso
    // ni sonido cada vez que se activa o desactiva con ella.
    virtual bool QuietKey() const { return false; }

    // --- Lo que cada módulo puede reemplazar ----------------------------------

    // Su ícono: un path SVG en una cuadrícula de 24x24 (ver render/icons.h).
    // nullptr = el ícono de su categoría.
    virtual const char* IconSvg() const { return nullptr; }

    // ¿Dibuja algo en pantalla cada frame mientras está activo?
    virtual bool HasHud() const { return false; }
    // ¿Es un "elemento" del HUD que se puede mover y escalar con el editor?
    // (La mira, por ejemplo, dibuja pero siempre en el centro: no lo es.)
    virtual bool IsHudElement() const { return HasHud(); }
    // ¿Se le puede cambiar el tamaño en el editor de HUD? (No, por ejemplo, si
    // lo dibuja el juego y Luma solo lo mueve, como el scoreboard.)
    virtual bool CanScale() const { return true; }
    // Dibuja su parte del HUD (dentro de un frame de ImGui) con su esquina
    // superior izquierda en frame.pos, y devuelve el tamaño que ocupó (el
    // editor lo usa para saber dónde está y el gestor para ubicarlo).
    virtual ImVec2 OnRenderHud(const HudFrame& frame) { (void)frame; return ImVec2(0, 0); }

    // Posición y escala en pantalla (las cambia el editor de HUD).
    HudLayout& Layout() { return layout_; }
    const HudLayout& DefaultLayout() const { return defaultLayout_; }
    // Rectángulo que ocupó en el último frame (en píxeles de pantalla).
    // Estilo propio del elemento (si customStyle es false, usa el global).
    bool& CustomStyle() { return customStyle_; }
    render::hud::Style& ElementStyle() { return style_; }
    ImVec2 LastPos() const { return lastPos_; }
    ImVec2 LastSize() const { return lastSize_; }

    // ¿Necesita revisar algo periódicamente aunque no dibuje? (por ejemplo,
    // el aviso de durabilidad). OnTick se llama cada ~50 ms desde el hilo
    // principal de Luma, solo mientras el módulo está activo.
    virtual bool HasTick() const { return false; }
    virtual void OnTick() {}

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
    // Cada módulo con HUD fija su posición de fábrica en su constructor con
    // SetDefaultLayout (así "Restablecer" vuelve a ella).
    // Tecla de fábrica (la que tiene si el perfil no guardó otra).
    void SetDefaultKeybind(int vk) {
        keybind_ = vk;
        defaultKeybind_ = vk;
    }
    void SetDefaultLayout(HudLayout layout) {
        layout_ = layout;
        defaultLayout_ = layout;
    }

private:
    std::string id_;
    std::string name_;
    std::string description_;
    Category category_;
    bool enabled_ = false;
    int keybind_ = keys::kNone;
    int defaultKeybind_ = keys::kNone;
    HudLayout layout_;
    HudLayout defaultLayout_;
    bool customStyle_ = false;
    render::hud::Style style_;
    ImVec2 lastPos_{0, 0};
    ImVec2 lastSize_{0, 0};
};

}  // namespace luma::modules
