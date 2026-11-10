// =============================================================================
//  flow.h - Lo que pasa al pulsar JUGAR (sin nada de interfaz)
// =============================================================================
//  Separado de la ventana para que la interfaz solo MUESTRE el estado:
//
//    1. Ubicar Luma.dll (junto al launcher).
//    2. Si Minecraft no está abierto, abrirlo y esperar a que muestre su
//       ventana (y unos segundos más, para que termine de arrancar).
//    3. Comprobar que es la versión GDK y que Luma no esté ya cargado.
//    4. Inyectar Luma.dll y confirmar que escribió su log.
//
//  Todo corre en un hilo aparte (PlayFlow::Start no bloquea la ventana): la
//  interfaz pregunta el estado cada frame con Snapshot().
// =============================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luma::launcher {

enum class PlayState {
    Idle,           // sin hacer nada
    LaunchingGame,  // pidiéndole a Windows que abra Minecraft
    WaitingForGame, // esperando a que Minecraft cargue
    Injecting,
    Verifying,      // esperando el log de Luma
    Done,           // Luma cargado
    AlreadyLoaded,  // ya estaba cargado (no es un error)
    Unsupported,    // esta versión de Minecraft no está en la lista de Luma: pedir confirmación
    Failed,
};

struct PlaySnapshot {
    PlayState state = PlayState::Idle;
    std::string messageKey;  // clave de traducción del mensaje (i18n::T)
    std::string detail;      // dato técnico extra (rutas, código de error), sin traducir
};

class PlayFlow {
public:
    ~PlayFlow();
    // Empieza (si no está ya en marcha). "supportedMinecraft": las versiones
    // que soporta esta Luma (vacío = no se sabe, no avisar). "force": jugar
    // igual aunque la versión no esté en la lista.
    void Start(std::vector<std::string> supportedMinecraft = {}, bool force = false);
    PlaySnapshot Snapshot() const;
    bool Busy() const;

private:
    void Run(const std::vector<std::string>& supported, bool force);
    void Set(PlayState state, const char* key, std::string detail = {});

    mutable std::mutex mutex_;
    PlaySnapshot snapshot_;
    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> quitting_{false};
};

// --- Estado del juego (para la tarjeta "Info del cliente") ----------------------
struct GameStatus {
    bool running = false;
    bool lumaLoaded = false;
    std::string gameVersion;  // "1.26.63" ("" si no está instalado o no se supo)
};

// Consulta el estado. Barata (lista procesos y módulos): se llama cada ~1,5 s
// desde un hilo aparte, nunca desde el frame.
GameStatus QueryGameStatus();

}  // namespace luma::launcher
