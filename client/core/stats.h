// =============================================================================
//  stats.h - Estadísticas de juego y nombre del jugador (para el launcher)
// =============================================================================
//  Luma anota dos archivos en %APPDATA%\Luma\ que lee el launcher:
//
//    stats.json   -> {"sessions": 12, "playSeconds": 45210}
//                    Una "sesión" = una vez que Luma se cargó en el juego. El
//                    tiempo cuenta solo mientras Luma está cargado. Se guarda
//                    cada minuto (si el juego se cierra de golpe, se pierde
//                    como mucho un minuto) y al descargar Luma.
//    profile.json -> {"name": "TuGamertag"}
//                    El nombre de tu jugador (en Bedrock es tu gamertag de
//                    Xbox), leído del juego al entrar a un mundo.
//
//  Todo corre en el hilo principal de Luma (el que espera la tecla de
//  descarga), nunca en el de render: escribir en disco ahí costaría frames.
// =============================================================================
#pragma once

namespace luma::stats {

// Al cargar Luma: cuenta una sesión nueva.
void Begin();
// Cada vuelta del hilo principal (~50 ms): hace algo solo cada tanto.
void Poll();
// Al descargar Luma: guarda el tiempo que faltaba.
void End();

}  // namespace luma::stats
