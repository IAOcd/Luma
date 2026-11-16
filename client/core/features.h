// =============================================================================
//  features.h - Qué partes de Luma están encendidas
// =============================================================================
//  LUMA ES UN CLIENT DE MÚSICA. Desde la 0.7 (2026-11), Luma se enfoca en
//  una sola cosa: la música dentro de Minecraft (Now Playing, letras, Lyric
//  Flow, el visualizador, la página de Música y el Halo del radial), y está
//  pensada para usarse JUNTO a otros clients (Flarial, por ahora).
//
//  Todo lo demás (HUD general, zoom, freelook, free cam, minimapa,
//  hitboxes, toggle sprint, optimizaciones, V-Sync off, Closet...) SIGUE EN
//  EL CÓDIGO, pero apagado: no se registra, no aparece en el menú y no
//  engancha nada del juego. Así no choca con lo que ya hace el otro client,
//  y cuesta menos. Para volver a encenderlo todo: kMusicOnly = false.
// =============================================================================
#pragma once

namespace luma::features {

// true = solo la música (lo demás queda apagado y oculto).
inline constexpr bool kMusicOnly = true;

}  // namespace luma::features
