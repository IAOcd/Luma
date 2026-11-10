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
//  Qué textura usa cada ítem lo dicen las mismas tablas del juego:
//    - Ítems: el ítem guarda el nombre de su ícono (Item+0x68, por ejemplo
//      "apple_golden" o "helmet") y textures/item_texture.json dice qué
//      archivo es. Si hay variantes (todos los cascos), elegimos la que
//      coincide con el id ("leather_helmet").
//    - Bloques: blocks.json dice qué textura usa cada cara, y
//      textures/terrain_texture.json dónde está.
//  Formatos: PNG (con WIC, de Windows) y TGA (lo leemos nosotros). El cuero
//  se tiñe con su color (o el marrón de fábrica), como en el juego.
//
//  HILOS: Get() desde el render (con el candado); Poll() desde el hilo
//  principal de Luma (lee y decodifica: nunca frena el render);
//  UploadPending() desde el render (sube a la GPU).
// =============================================================================
#pragma once

#include <imgui.h>

namespace luma::render::icons_items {

// La textura del ícono de un ítem, o 0 si todavía no está (la pide y estará
// en unos milisegundos) o no existe.
//   id    = "minecraft:leather_helmet"
//   atlas = el nombre de ícono que guarda el propio ítem del juego ("helmet");
//           con él se usa la misma tabla que el juego (item_texture.json).
//           nullptr = buscar por el id.
//   color = color de teñido (armadura de cuero), -1 = sin teñir.
ImTextureID Get(const char* id, const char* atlas = nullptr, int color = -1);

// Hilo principal: procesa los pedidos (lee del disco y decodifica).
void Poll();

// Render: sube a la GPU los íconos ya decodificados.
void UploadPending();

}  // namespace luma::render::icons_items
