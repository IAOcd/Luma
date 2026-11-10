// =============================================================================
//  skin_pack.h - Tus skins de Luma como un "paquete de skins" del juego
// =============================================================================
//  El problema: Minecraft no tiene una forma pública de que otro programa le
//  cambie la skin. Lo que SÍ acepta son los paquetes de skins (lo mismo que se
//  instala con un .mcpack): una carpeta en
//    %APPDATA%\Minecraft Bedrock\Users\Shared\games\com.mojang\skin_packs\
//  con un manifest.json, un skins.json y los PNG.
//
//  Luma mantiene ahí un paquete llamado "Luma" con toda tu biblioteca
//  (%APPDATA%\Luma\skins\), con la skin EQUIPADA primero. En el Vestidor del
//  juego aparece como un paquete más: eliges la skin una vez y queda puesta
//  (también en servidores, como cualquier skin personalizada).
//
//  Límite honesto: el juego lee los paquetes al abrirse. Una skin nueva en la
//  biblioteca aparece en el Vestidor después de reiniciar Minecraft.
//
//  Lo usan el launcher (al equipar/importar/borrar) y Luma.dll (Closet).
// =============================================================================
#pragma once

namespace luma::skinpack {

// Reescribe el paquete con lo que hay ahora en la biblioteca. Devuelve false
// si no encontró la carpeta del juego o no pudo escribir.
bool Write();

}  // namespace luma::skinpack
