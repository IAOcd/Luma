// =============================================================================
//  account.h - Iniciar sesión con Microsoft (cuenta de Xbox), OPCIONAL
// =============================================================================
//  Para qué: mostrar tu gamertag y tu foto de Xbox en el launcher. Jugar NO
//  depende de esto: sin sesión, el nombre sale del juego (profile.json) y si
//  los servidores de Microsoft fallan, Luma funciona igual.
//
//  CÓMO FUNCIONA (el estándar "OAuth 2.0 con PKCE" para programas de escritorio):
//    1. El launcher abre TU navegador en la página de Microsoft. Tu contraseña
//       se escribe ahí, nunca en Luma.
//    2. Al aceptar, Microsoft redirige el navegador a http://localhost:<puerto>
//       con un "código". El launcher escucha en ese puerto un instante y lo
//       recibe. PKCE = un secreto de un solo uso que inventa el launcher, para
//       que nadie más pueda canjear ese código.
//    3. Código -> "token" de Microsoft -> token de Xbox Live -> token XSTS,
//       que trae tu gamertag. Con él se pide tu foto de perfil.
//    4. Para no pedirte iniciar sesión cada vez se guarda el "refresh token"
//       en %APPDATA%\Luma\account.dat, CIFRADO con DPAPI (la protección de
//       Windows: solo tu usuario de Windows en esta PC puede descifrarlo).
//
//  La aplicación "Luma Launcher" está registrada en Azure (el identificador de
//  abajo, kClientId, es público por diseño: no da acceso a nada por sí solo).
//  Solo hablamos con servidores de Microsoft y Xbox.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luma::launcher::account {

enum class State {
    SignedOut,
    SigningIn,   // esperando al navegador / pidiendo tokens
    Refreshing,  // al abrir: renovando la sesión guardada
    SignedIn,
    Error,       // el último intento falló (errorKey dice por qué)
};

struct Info {
    State state = State::SignedOut;
    std::string gamertag;
    std::string errorKey;  // clave de traducción
    std::string detail;    // dato técnico, sin traducir
    // Foto de perfil ya decodificada (RGBA, 4 bytes por píxel). "pictureVersion"
    // cambia cada vez que llega una foto nueva (para rehacer la textura).
    std::vector<std::uint8_t> picture;
    int pictureWidth = 0;
    int pictureHeight = 0;
    int pictureVersion = 0;
};

// Al abrir el launcher: si hay una sesión guardada, la renueva (en segundo plano).
void Init();
// Botón "Iniciar sesión": abre el navegador (en segundo plano).
void SignIn();
// Cancelar el inicio de sesión en curso (deja de esperar al navegador).
void Cancel();
// Cerrar sesión: borra la sesión guardada.
void SignOut();
// El estado. "includePicture" = también copiar la foto (solo cuando cambió su versión).
Info Get(bool includePicture = false);
void Shutdown();

}  // namespace luma::launcher::account
