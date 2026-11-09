// =============================================================================
//  config.h - Configuración y perfiles de Luma (JSON)
// =============================================================================
//  Dónde se guarda:
//    %APPDATA%\Luma\config\luma.json              -> qué perfil está activo
//    %APPDATA%\Luma\config\profiles\<nombre>.json -> todo lo demás
//
//  Un PERFIL es una configuración completa (módulos activos, sus opciones y
//  teclas, el tema, las teclas del menú y de descarga). Puedes tener varios
//  ("PvP", "Survival", ...) y cambiar entre ellos desde el menú.
//
//  Guardado automático: cuando algo cambia se marca "pendiente de guardar" y
//  el hilo principal de Luma lo escribe un segundo después (así mover un
//  slider no escribe el archivo 60 veces por segundo). Se escribe primero a
//  un archivo temporal y después se reemplaza el real: si el juego se cierra
//  justo en ese momento, el archivo nunca queda a medio escribir.
//
//  Si un archivo está dañado, se guarda una copia (.corrupto.json), se usan
//  los valores por defecto y se avisa con un toast. Nunca se crashea.
//
//  HILOS: salvo donde se indique, llamar con luma::StateMutex() tomado.
// =============================================================================
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace luma::config {

// Teclas globales (dentro del perfil). Atómicas porque las leen la ventana y
// el hilo principal sin tomar el candado.
extern std::atomic<int> g_menuKey;
extern std::atomic<int> g_unloadKey;

// Carga el perfil activo (al arrancar, desde el hilo principal).
void LoadAtStartup();

// Marca que hay cambios para guardar (cualquier hilo, sin candado).
void MarkDirty();

// Guarda si hay cambios pendientes desde hace más de 1 s. Hilo principal,
// SIN el candado tomado (lo toma solo lo justo).
void SaveIfDirty();

// Guarda ya (al descargar Luma). Hilo principal, sin el candado tomado.
void SaveNow();

// --- Perfiles ------------------------------------------------------------------
const std::string& ActiveProfile();
std::vector<std::string> ListProfiles();

// Nombre válido: letras, números, espacios, '-' y '_', de 1 a 32 caracteres.
bool IsValidProfileName(const std::string& name);

// Guarda el perfil actual y carga otro.
bool SwitchProfile(const std::string& name);
// Crea un perfil nuevo copiando la configuración actual y lo activa.
bool CreateProfile(const std::string& name);
// Borra un perfil (no el activo).
bool DeleteProfile(const std::string& name);

// Vuelve todo a los valores de fábrica en el perfil activo.
void ResetActiveProfile();

}  // namespace luma::config
