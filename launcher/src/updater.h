// =============================================================================
//  updater.h - Actualizaciones automáticas del launcher (como Flarial)
// =============================================================================
//  Descargas UN .exe y él se mantiene al día solo:
//
//    1. Al abrir, baja de GitHub (la última "Release" de IAOcd/Luma) dos
//       archivos chicos: latest.json (qué versión es la última, su huella,
//       notas, versiones de Minecraft soportadas) y latest.json.sig (su firma).
//    2. Comprueba la firma con la clave pública de Luma (signature.cpp). Si no
//       coincide, no se usa nada de ese archivo.
//    3. Si la versión es más nueva: baja el .exe nuevo junto al actual
//       ("LumaLauncher.exe.new"), comprueba su huella SHA-256 y lo cambia:
//       actual -> "LumaLauncher.exe.old", nuevo -> "LumaLauncher.exe". Windows
//       deja renombrar un programa abierto. Abre el nuevo y se cierra.
//    4. El nuevo borra el ".old" al arrancar.
//
//  Sin internet, o si GitHub no responde, el launcher sigue normal. Las
//  compilaciones locales (las tuyas, desde build\) NUNCA se reemplazan solas:
//  solo las oficiales, que compila GitHub Actions (LUMA_OFFICIAL_BUILD).
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace luma::launcher::updater {

enum class State {
    Idle,
    Checking,
    UpToDate,
    Available,    // hay una nueva y no se instala sola (opción apagada o compilación local)
    Downloading,
    Installing,
    Restarting,   // la nueva ya se abrió: la interfaz tiene que cerrarse
    Offline,      // no se pudo consultar (sin internet, sin Releases todavía...)
    Failed,       // algo salió mal al actualizar (se sigue con la versión actual)
};

struct Note {
    std::string title;
    std::string body;
};

struct Info {
    State state = State::Idle;
    std::string latestVersion;
    float progress = 0.0f;                  // 0..1 mientras descarga
    std::string errorKey;
    std::string detail;
    std::string date;                       // fecha de la última versión
    std::vector<Note> notes;                // novedades en el idioma actual
    std::vector<std::string> supportedMinecraft;  // vacío = no se sabe (no avisar)
    std::string updatedFrom;                // "" o la versión de la que venimos (recién actualizado)
};

// Al abrir el launcher (en segundo plano). "autoInstall": instalar sin preguntar.
// "commandLine": para saber si venimos de una actualización (--updated-from=X).
void Start(bool autoInstall, const wchar_t* commandLine);
// Botón "Buscar ahora" / "Actualizar ahora".
void CheckNow();
void InstallNow();
void SetAutoInstall(bool enabled);
Info Get();
// ¿Es una compilación oficial (de GitHub Actions)? Solo esas se actualizan solas.
bool OfficialBuild();
void Shutdown();

}  // namespace luma::launcher::updater
