// =============================================================================
//  i18n.h - Traducciones de Luma (inglés, español y portugués)
// =============================================================================
//  "i18n" es la abreviatura habitual de "internationalization" (una i, 18
//  letras, una n). La idea: el código nunca escribe un texto directamente,
//  sino una CLAVE ("home.play"), y esta función devuelve el texto en el
//  idioma elegido:
//
//      ImGui::Text("%s", i18n::T("home.play"));   // "PLAY" / "JUGAR" / "JOGAR"
//
//  Las traducciones están en i18n.cpp, una línea por texto con los tres
//  idiomas juntos (así es difícil olvidarse de uno). Si falta una traducción
//  se usa el inglés, y si falta la clave entera se muestra la clave (así se
//  nota enseguida en pantalla).
//
//  Esta carpeta (/shared) la usan el launcher y, más adelante, el menú del
//  juego: los dos comparten los mismos textos y la misma opción de idioma.
// =============================================================================
#pragma once

namespace luma::i18n {

enum class Language { English = 0, Spanish = 1, Portuguese = 2 };
inline constexpr int kLanguageCount = 3;

void SetLanguage(Language language);
Language CurrentLanguage();

// Código corto para guardar en la config: "en", "es", "pt".
const char* LanguageCode(Language language);
Language LanguageFromCode(const char* code);  // inglés si no lo reconoce
// Nombre del idioma escrito en ese mismo idioma ("English", "Español"...).
const char* LanguageName(Language language);

// El texto de "key" en el idioma actual.
const char* T(const char* key);

}  // namespace luma::i18n
