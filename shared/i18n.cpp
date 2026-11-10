// =============================================================================
//  i18n.cpp - La tabla de traducciones (ver i18n.h)
// =============================================================================
//  PARA AGREGAR UN TEXTO: una línea nueva en kEntries con la clave y los tres
//  idiomas. Las claves se agrupan por pantalla ("home.", "nav.", ...).
//  "%s" dentro de un texto = un dato que el programa completa (un nombre, una
//  versión...). Tiene que estar en los tres idiomas.
// =============================================================================
#include "i18n.h"

#include <atomic>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace luma::i18n {

namespace {

struct Entry {
    const char* key;
    const char* text[kLanguageCount];  // inglés, español, portugués
};

// clang-format off
constexpr Entry kEntries[] = {
    // --- Barra lateral -------------------------------------------------------------
    {"nav.home",            {"Home", "Inicio", "Início"}},
    {"nav.addons",          {"Add-ons", "Add-ons", "Add-ons"}},
    {"nav.skins",           {"Skins", "Skins", "Skins"}},
    {"nav.settings",        {"Settings", "Ajustes", "Configurações"}},
    {"nav.guest",           {"Player", "Jugador", "Jogador"}},
    {"nav.not_signed_in",   {"Not signed in", "Sin iniciar sesión", "Sem sessão iniciada"}},

    // --- Barra de título ----------------------------------------------------------
    {"title.theme_dark",    {"Dark theme", "Tema oscuro", "Tema escuro"}},
    {"title.theme_light",   {"Light theme", "Tema claro", "Tema claro"}},
    {"title.minimize",      {"Minimize", "Minimizar", "Minimizar"}},
    {"title.maximize",      {"Maximize", "Maximizar", "Maximizar"}},
    {"title.restore",       {"Restore", "Restaurar", "Restaurar"}},
    {"title.close",         {"Close", "Cerrar", "Fechar"}},

    // --- Inicio -----------------------------------------------------------------------
    {"home.welcome",        {"Welcome back,", "Bienvenido de nuevo,", "Bem-vindo de volta,"}},
    {"home.subtitle",       {"Ready to explore new worlds?", "¿Listo para explorar nuevos mundos?",
                             "Pronto para explorar novos mundos?"}},
    {"home.play",           {"PLAY", "JUGAR", "JOGAR"}},
    {"home.quick_access",   {"Quick Access", "Acceso rápido", "Acesso rápido"}},
    {"home.qa_addons",      {"Enhance your game", "Mejora tu juego", "Melhore seu jogo"}},
    {"home.qa_skins",       {"Customize your look", "Personaliza tu aspecto", "Personalize seu visual"}},
    {"home.qa_settings",    {"Configure Luma", "Configura Luma", "Configure o Luma"}},
    {"home.client_info",    {"Client Info", "Info del cliente", "Info do cliente"}},
    {"home.luma_version",   {"Luma version", "Versión de Luma", "Versão do Luma"}},
    {"home.game_version",   {"Minecraft version", "Versión de Minecraft", "Versão do Minecraft"}},
    {"home.game",           {"Minecraft", "Minecraft", "Minecraft"}},
    {"home.luma",           {"Luma", "Luma", "Luma"}},
    {"home.not_installed",  {"Not found", "No encontrado", "Não encontrado"}},
    {"home.game_running",   {"Running", "Abierto", "Aberto"}},
    {"home.game_closed",    {"Closed", "Cerrado", "Fechado"}},
    {"home.luma_loaded",    {"Loaded in the game", "Cargado en el juego", "Carregado no jogo"}},
    {"home.luma_unloaded",  {"Not loaded", "Sin cargar", "Não carregado"}},
    {"home.banner",         {"Play better. Your way.", "Juega mejor. A tu manera.", "Jogue melhor. Do seu jeito."}},
    {"home.ready",          {"Luma is ready", "Luma está listo", "Luma está pronto"}},

    // --- Estado de JUGAR ---------------------------------------------------------------
    {"play.launching",      {"Opening Minecraft...", "Abriendo Minecraft...", "Abrindo o Minecraft..."}},
    {"play.waiting",        {"Waiting for Minecraft to load...", "Esperando a que Minecraft cargue...",
                             "Esperando o Minecraft carregar..."}},
    {"play.injecting",      {"Loading Luma into the game...", "Cargando Luma en el juego...",
                             "Carregando o Luma no jogo..."}},
    {"play.verifying",      {"Checking that Luma started...", "Comprobando que Luma arrancó...",
                             "Verificando se o Luma iniciou..."}},
    {"play.done",           {"Luma is running. Have fun!", "Luma está funcionando. ¡A jugar!",
                             "O Luma está rodando. Divirta-se!"}},
    {"play.already",        {"Luma is already running in the game.", "Luma ya está funcionando en el juego.",
                             "O Luma já está rodando no jogo."}},
    {"play.done_no_log",    {"Luma was loaded, but its log was not found.", "Luma se cargó, pero no se encontró su log.",
                             "O Luma foi carregado, mas o log não foi encontrado."}},
    {"play.unload_hint",    {"Press End in the game to unload Luma.", "Pulsa Fin en el juego para descargar Luma.",
                             "Pressione End no jogo para descarregar o Luma."}},

    // --- Errores --------------------------------------------------------------------------
    {"error.title",         {"Something went wrong", "Algo salió mal", "Algo deu errado"}},
    {"error.no_dll",        {"Luma.dll could not be prepared. Try running the launcher again.",
                             "No se pudo preparar Luma.dll. Prueba abrir el launcher otra vez.",
                             "Não foi possível preparar a Luma.dll. Tente abrir o launcher de novo."}},
    {"error.launch_failed", {"Minecraft could not be opened. Open it yourself and press PLAY again.",
                             "No se pudo abrir Minecraft. Ábrelo tú y vuelve a pulsar JUGAR.",
                             "Não foi possível abrir o Minecraft. Abra-o você mesmo e pressione JOGAR de novo."}},
    {"error.game_timeout",  {"Minecraft took too long to open.", "Minecraft tardó demasiado en abrir.",
                             "O Minecraft demorou demais para abrir."}},
    {"error.game_closed",   {"Minecraft was closed before Luma could load.", "Minecraft se cerró antes de que Luma cargara.",
                             "O Minecraft foi fechado antes de o Luma carregar."}},
    {"error.query",         {"The game process could not be checked.", "No se pudo consultar el proceso del juego.",
                             "Não foi possível verificar o processo do jogo."}},
    {"error.uwp",           {"This is the old UWP version of Minecraft. Luma only supports the current (GDK) version: "
                             "update it from the Microsoft Store or the Xbox app.",
                             "Este es el Minecraft UWP (antiguo). Luma solo funciona con la versión actual (GDK): "
                             "actualízalo desde la Microsoft Store o la app Xbox.",
                             "Este é o Minecraft UWP (antigo). O Luma só funciona com a versão atual (GDK): "
                             "atualize pela Microsoft Store ou pelo app Xbox."}},
    {"error.not_gdk",       {"Could not confirm that this Minecraft is the GDK version.",
                             "No se pudo confirmar que este Minecraft sea la versión GDK.",
                             "Não foi possível confirmar que este Minecraft é a versão GDK."}},
    {"error.inject",        {"Luma could not be loaded into the game.", "No se pudo cargar Luma en el juego.",
                             "Não foi possível carregar o Luma no jogo."}},
    {"error.retry",         {"Try again", "Reintentar", "Tentar de novo"}},

    // --- Secciones que aún no existen ---------------------------------------------
    {"soon.title",          {"Coming soon", "Próximamente", "Em breve"}},
    {"soon.addons",         {"Manage your resource and behavior packs from here.",
                             "Desde aquí vas a manejar tus resource packs y behavior packs.",
                             "Daqui você vai gerenciar seus resource packs e behavior packs."}},
    {"soon.skins",          {"Preview and organize your skins in 3D.", "Mira y organiza tus skins en 3D.",
                             "Veja e organize suas skins em 3D."}},
    {"soon.settings",       {"Language, theme, startup and more.", "Idioma, tema, inicio con Windows y más.",
                             "Idioma, tema, inicialização e mais."}},
};
// clang-format on

std::atomic<int> g_language{static_cast<int>(Language::English)};

// Índice clave -> entrada, armado una sola vez (después, buscar no pide memoria).
const std::unordered_map<std::string_view, const Entry*>& Index() {
    static const std::unordered_map<std::string_view, const Entry*> index = [] {
        std::unordered_map<std::string_view, const Entry*> map;
        map.reserve(std::size(kEntries));
        for (const Entry& e : kEntries) {
            map.emplace(e.key, &e);
        }
        return map;
    }();
    return index;
}

}  // namespace

void SetLanguage(Language language) {
    g_language.store(static_cast<int>(language), std::memory_order_relaxed);
}

Language CurrentLanguage() {
    return static_cast<Language>(g_language.load(std::memory_order_relaxed));
}

const char* LanguageCode(Language language) {
    switch (language) {
        case Language::Spanish: return "es";
        case Language::Portuguese: return "pt";
        default: return "en";
    }
}

Language LanguageFromCode(const char* code) {
    if (code != nullptr && std::strcmp(code, "es") == 0) return Language::Spanish;
    if (code != nullptr && std::strcmp(code, "pt") == 0) return Language::Portuguese;
    return Language::English;
}

const char* LanguageName(Language language) {
    switch (language) {
        case Language::Spanish: return "Español";
        case Language::Portuguese: return "Português";
        default: return "English";
    }
}

const char* T(const char* key) {
    const auto& index = Index();
    const auto it = index.find(key);
    if (it == index.end()) {
        return key;  // falta la clave: se ve en pantalla y se corrige
    }
    const char* text = it->second->text[static_cast<int>(CurrentLanguage())];
    return text != nullptr && text[0] != '\0' ? text : it->second->text[0];
}

}  // namespace luma::i18n
