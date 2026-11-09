// =============================================================================
//  item_icons.h - Íconos de los ítems, leídos de TU instalación del juego
// =============================================================================
//  Igual que la fuente pixelada: Luma no incluye imágenes del juego. Las lee
//  al vuelo de los paquetes de recursos que trae tu Minecraft:
//
//    Content\data\resource_packs\vanilla*\__brarchive\textures\items.brarchive
//                                                             \blocks.brarchive
//
//  Un ".brarchive" es un paquete simple (lo desciframos leyéndolo):
//    - 16 bytes de cabecera: firma (8), cantidad de archivos (4), versión (4)
//    - una entrada de 256 bytes por archivo: largo del nombre (1), nombre
//      (247), posición (4) y tamaño (4) de sus datos
//    - después, los archivos (aquí, PNG normales) uno tras otro.
//  Hay un paquete por versión del juego (vanilla_1.26.63, ...): cada uno
//  agrega o reemplaza texturas. Usamos la versión más nueva de cada una.
//
//  El nombre de la textura casi siempre es el del ítem ("minecraft:diamond"
//  -> diamond.png). Para los que no coinciden hay una tabla pequeña (ver
//  .cpp). Si no se encuentra, no hay ícono (el módulo muestra solo texto).
//
//  HILOS: Get() desde el render (con el candado); Poll() desde el hilo
//  principal de Luma (lee y decodifica: nunca frena el render);
//  UploadPending() desde el render (sube a la GPU).
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::icons_items {

// La textura del ícono de "id" ("minecraft:diamond"), o 0 si todavía no
// está (la pide y estará en unos milisegundos) o no existe.
ImTextureID Get(const char* id);

// Hilo principal: procesa los pedidos (lee del disco y decodifica).
void Poll();

// Render: sube a la GPU los íconos ya decodificados.
void UploadPending();

}  // namespace luma::render::icons_items
