// =============================================================================
//  skin_apply.h - Cambiar la skin del jugador AL INSTANTE (sin el Vestidor)
// =============================================================================
//  Cómo cambia la skin el juego (investigado con puntos de vigilancia de
//  memoria en 1.26.63, ver sdk/watch.h):
//    El Vestidor NO cambia la skin directamente: le manda al servidor un
//    paquete PlayerSkinPacket (número 93) con la skin nueva. El servidor (en
//    un mundo local, tu propio juego) la aplica y se la reenvía a todos.
//
//  Luma hace lo mismo:
//    1. Crea un paquete 93 con MinecraftPackets::createPacket (del juego).
//    2. Le pone tu UUID (de la lista de jugadores) y, SOLO mientras se manda,
//       una copia "superficial" de tu skin actual con los píxeles de la skin
//       equipada y un identificador nuevo (así el juego crea una textura nueva).
//    3. Lo manda con PacketSender::sendToServer (Player+0x820, función 4).
//    4. Le devuelve al paquete su contenido original y lo suelta.
//
//  Límites honestos:
//    - Solo skins del mismo tamaño que la actual (64x64 con 64x64...).
//    - Los brazos (finos o anchos) quedan como los de la skin actual.
//    - Los servidores que bloquean cambiar la skin a mitad de partida la
//      rechazan (igual que con el Vestidor).
// =============================================================================
#pragma once

namespace luma::sdk::skinapply {

bool Init();
bool Available();

// Para otras partes del SDK (sdk/cape): crear un paquete del juego por su
// número (MinecraftPackets::createPacket) y soltarlo. Es un std::shared_ptr:
// "object" es el paquete y "control" su contador.
struct PacketRef {
    void* object = nullptr;
    void* control = nullptr;
};
bool CreatePacket(int id, PacketRef& out);
void ReleasePacket(PacketRef& packet);

// Hilo principal de Luma (cada 50 ms): si cambió la skin equipada
// (skins\active.json, la escriben el Closet y el launcher), lee su PNG.
void Poll();

// Hilo de la interfaz del juego, ANTES de dibujar (lo llama el hook de
// ScreenView::setupAndRender): si hay una skin pendiente, manda el paquete.
void OnUiFrame();

}  // namespace luma::sdk::skinapply
