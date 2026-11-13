// =============================================================================
//  cosmetics.h - Los cosméticos de Luma (por ahora, las capas)
// =============================================================================
//  Son parte de Luma, no un módulo que se activa: eliges tu capa en el
//  Closet (sección "Cosméticos") y Luma se encarga de ponértela cada vez que
//  entras a un mundo (y de volver a ponerla si el juego te devuelve tu skin
//  original, por ejemplo al cambiar de dimensión).
//
//  DÓNDE SE VE: por ahora, en tus mundos locales (sdk/cape.h explica cómo y
//  por qué). En servidores, y que la vean otros usuarios de Luma (el servidor
//  de Luma les dice cuál elegiste), es el paso siguiente.
//
//  La elección se guarda en %APPDATA%\Luma\cosmetics.json.
//
//  Diseños: algunos dibujados por Luma en código (pixel art de 64x32) y otros
//  PNG HD (256x128) que viajan DENTRO de Luma.dll (client/assets/capes).
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::cosmetics {

// EL INTERRUPTOR: los cosméticos (capas) están EN DESARROLLO y apagados. Con
// false, Luma no los inicia (no engancha nada del juego para ellos, no toca
// tu skin) y el Closet muestra "Próximamente". Para trabajar en ellos: true.
inline constexpr bool kEnabled = false;

// Los diseños de capa. El id es el que guardará el servidor de Luma
// (/v1/me/cosmetics).
struct CapeDesign {
    const char* id;
    const char* nameKey;  // texto del menú (shared/i18n)
    int resource;         // 0 = dibujada por Luma en código; si no, el PNG dentro de Luma.dll (client/assets/luma.rc)
    // Animadas: un segundo PNG con la cara de AFUERA de cada cuadro (40x64),
    // en una grilla de "columns" columnas. 0 = no animada.
    int animResource = 0;
    int frames = 0;
    int columns = 0;
    int frameMs = 0;      // cuánto dura cada cuadro
};
inline constexpr CapeDesign kCapes[] = {
    {"luma_classic", "closet.cape_classic", 0},
    {"luma_dawn", "closet.cape_dawn", 0},
    {"luma_night", "closet.cape_night", 0},
    {"luma_grid", "closet.cape_grid", 201, 204, 450, 50, 70},
    {"luma_galaxy", "closet.cape_galaxy", 202},
    {"luma_apple", "closet.cape_apple", 203},
};

// La imagen de una capa (RGBA). Las dibujadas en código miden 64x32; las de
// PNG son "HD": 256x128 (4 veces más detalle; la misma forma).
struct CapeTexture {
    std::vector<std::uint8_t> rgba;
    unsigned width = 0;
    unsigned height = 0;
};

// La imagen de un diseño (vacía si el id no existe). Las de PNG se leen una
// sola vez y quedan guardadas.
CapeTexture CapeImage(const std::string& id);
// ¿Es un id de capa de Luma?
bool IsLumaCape(const std::string& id);

// Carga la elección guardada (al arrancar Luma).
void Load();
// Tu capa elegida ("" = ninguna).
std::string SelectedCape();
// Elegir capa ("" = ninguna). Se guarda y se aplica en un momento.
void SelectCape(const std::string& id);

// Hilo principal de Luma (cada 50 ms; trabaja una vez por segundo): pone o
// quita tu capa según lo elegido.
void Poll();

}  // namespace luma::cosmetics
