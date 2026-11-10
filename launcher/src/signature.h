// =============================================================================
//  signature.h - Comprobar que una actualización viene de verdad de Luma
// =============================================================================
//  QUÉ ES UNA FIRMA DIGITAL (ECDSA P-256):
//    Hay dos claves que van juntas. La PRIVADA solo la tiene quien publica
//    Luma (está en los "Secrets" de GitHub y en una copia de seguridad del
//    autor; nunca en el código). Con ella se "firma" el archivo latest.json.
//    La PÚBLICA va dentro del launcher (abajo, en signature.cpp) y solo sirve
//    para COMPROBAR firmas: no permite crear ninguna.
//
//    Si alguien cambiara latest.json (por ejemplo, para apuntar a un .exe
//    falso), la firma dejaría de coincidir y el launcher no lo instalaría.
//    latest.json además trae la huella SHA-256 del .exe: si el .exe cambia
//    aunque sea un byte, la huella no coincide.
// =============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace luma::launcher::signature {

// ¿"signature" (64 bytes: r y s) es una firma válida de "data" hecha con la
// clave privada de Luma?
bool Verify(const std::string& data, const std::string& signature);

// Huella SHA-256 de un archivo, en hexadecimal (64 caracteres). "" si falla.
std::string Sha256File(const std::filesystem::path& file);

}  // namespace luma::launcher::signature
