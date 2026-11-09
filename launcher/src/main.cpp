// =============================================================================
//  main.cpp - LumaLauncher.exe
// =============================================================================
//  Fase 0: solo imprime un mensaje para comprobar que compila.
//  En la Fase 1 aquí buscaremos el proceso del juego e inyectaremos Luma.dll.
// =============================================================================

// <cstdio> trae printf, una función para escribir texto en la consola.
#include <cstdio>

// main es la primera función que se ejecuta al abrir un programa.
int main() {
    std::printf("LumaLauncher (Fase 0): todavia no inyecta nada.\n");

    // Devolver 0 significa "terminó sin errores".
    return 0;
}
