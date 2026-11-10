// =============================================================================
//  skins.h - Tu biblioteca de skins (%APPDATA%\Luma\skins\)
// =============================================================================
//  Una carpeta con las skins (un PNG cada una) y library.json con sus datos
//  (nombre, favorita, de dónde vino). Dos formas de sumar skins:
//    - Del juego: cada vez que Luma.dll guarda una skin NUEVA tuya
//      (skin.png), se agrega sola ("Skin del juego").
//    - Importar: eliges un PNG de skin de Minecraft (64x64 o 64x32).
//
//  Más adelante, el menú de cosméticos de Luma (en el juego y en el launcher)
//  va a usar esta misma biblioteca.
//
//  Solo la usa el hilo de la interfaz (no hace falta candado).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::launcher::gui::skins {

struct Skin {
    std::string id;         // la huella SHA-256 del PNG (dos skins iguales = la misma)
    std::string name;
    std::wstring file;      // ruta del PNG
    bool slim = false;
    bool legacy = false;    // 64x32
    bool favorite = false;
    bool fromGame = false;  // vino del juego (si no, importada)
    std::int64_t added = 0; // cuándo se agregó (segundos desde 1970)
};

void Load();
const std::vector<Skin>& List();  // la más nueva primero

// Si "png" (la skin que guardó Luma.dll) no está en la biblioteca, la agrega.
// Devuelve el id de esa skin ("" si no se pudo).
std::string SyncFromGame(const std::wstring& png);

// Abre el diálogo de Windows para elegir un PNG y lo importa. "errorKey" = clave
// de traducción si falló (vacía si se canceló). Devuelve el id agregado.
std::string ImportWithDialog(void* ownerWindow, std::string& errorKey);

void SetFavorite(const std::string& id, bool favorite);
// La skin EQUIPADA de Luma (active.json, la misma que usa el Closet del juego).
std::string Active();
void SetActive(const std::string& id);
void Remove(const std::string& id);
std::wstring Folder();

}  // namespace luma::launcher::gui::skins
