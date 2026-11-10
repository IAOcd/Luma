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

namespace luma::launcher {

enum class PlayState {
    Idle,           // sin hacer nada
    LaunchingGame,  // pidiéndole a Windows que abra Minecraft
    WaitingForGame, // esperando a que Minecraft cargue
    Injecting,
    Verifying,      // esperando el log de Luma
    Done,           // Luma cargado
    AlreadyLoaded,  // ya estaba cargado (no es un error)
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
    // Empieza (si no está ya en marcha).
    void Start();
    PlaySnapshot Snapshot() const;
    bool Busy() const;

private:
    void Run();
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
