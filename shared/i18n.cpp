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
    {"home.subtitle",       {"Your Minecraft experience, reimagined.", "Tu experiencia de Minecraft, reinventada.",
                             "Sua experiência de Minecraft, reinventada."}},
    {"home.ready_to_play",  {"Ready to play", "Listo para jugar", "Pronto para jogar"}},
    {"home.platform",       {"Platform", "Plataforma", "Plataforma"}},
    {"play.menu_folder",    {"Open Luma folder", "Abrir carpeta de Luma", "Abrir pasta do Luma"}},
    {"play.menu_logs",      {"View logs", "Ver logs", "Ver logs"}},
    {"skin.title",          {"Your Skin", "Tu skin", "Sua skin"}},
    {"skin.placeholder",    {"Join a world with Luma to see your skin here.", "Entra a un mundo con Luma para ver tu skin aquí.",
                             "Entre em um mundo com o Luma para ver sua skin aqui."}},
    {"stats.last_played",   {"Last played", "Última partida", "Última partida"}},
    {"stats.today",         {"Today", "Hoy", "Hoje"}},
    {"stats.yesterday",     {"Yesterday", "Ayer", "Ontem"}},
    {"stats.days_ago",      {"%d days ago", "Hace %d días", "Há %d dias"}},
    {"stats.never",         {"Never", "Nunca", "Nunca"}},
    {"update.chip_ok",      {"Up to date", "Al día", "Atualizado"}},
    {"update.chip_new",     {"New version", "Versión nueva", "Versão nova"}},
    {"settings.sky",        {"Home sky", "Cielo del inicio", "Céu do início"}},
    {"sky.auto",            {"By time", "Según la hora", "Pela hora"}},
    {"sky.day",             {"Day", "Día", "Dia"}},
    {"sky.sunset",          {"Sunset", "Atardecer", "Pôr do sol"}},
    {"sky.night",           {"Night", "Noche", "Noite"}},
    {"skins.title",         {"My skins", "Mis skins", "Minhas skins"}},
    {"skins.all",           {"All", "Todas", "Todas"}},
    {"skins.favorites",     {"Favorites", "Favoritas", "Favoritas"}},
    {"skins.import",        {"Import", "Importar", "Importar"}},
    {"skins.import_error",  {"That image is not a Minecraft skin (it has to be 64x64 or 64x32).",
                             "Esa imagen no es una skin de Minecraft (tiene que ser de 64x64 o 64x32).",
                             "Essa imagem não é uma skin de Minecraft (precisa ser 64x64 ou 64x32)."}},
    {"skins.empty",         {"No skins yet. Join a world with Luma or import one.",
                             "Todavía no hay skins. Entra a un mundo con Luma o importa una.",
                             "Ainda não há skins. Entre em um mundo com o Luma ou importe uma."}},
    {"skins.no_favorites",  {"No favorites yet: open a skin and mark it.", "Sin favoritas todavía: abre una skin y márcala.",
                             "Sem favoritas ainda: abra uma skin e marque-a."}},
    {"skins.current",       {"In game", "En el juego", "No jogo"}},
    {"skins.from_game",     {"From the game", "Del juego", "Do jogo"}},
    {"skins.imported",      {"Imported", "Importada", "Importada"}},
    {"skins.slim",          {"Slim arms", "Brazos finos", "Braços finos"}},
    {"skins.classic",       {"Classic arms", "Brazos anchos", "Braços largos"}},
    {"skins.legacy",        {"Old format (64x32)", "Formato viejo (64x32)", "Formato antigo (64x32)"}},
    {"skins.favorite",      {"Add to favorites", "Marcar favorita", "Marcar favorita"}},
    {"skins.unfavorite",    {"Remove favorite", "Quitar favorita", "Tirar favorita"}},
    {"skins.open_folder",   {"Open folder", "Abrir carpeta", "Abrir pasta"}},
    {"skins.delete",        {"Delete", "Borrar", "Excluir"}},
    {"skins.confirm_delete", {"Click again to delete", "Clic otra vez para borrar", "Clique de novo para excluir"}},
    {"skins.use_in_game",   {"Use in Minecraft", "Usar en Minecraft", "Usar no Minecraft"}},
    {"skins.use_soon",      {"Coming with Luma cosmetics.", "Llega con los cosméticos de Luma.", "Chega com os cosméticos do Luma."}},
    {"skins.count_short",   {"%d skins", "%d skins", "%d skins"}},
    {"skins.count",         {"%d skins in your collection", "%d skins en tu colección", "%d skins na sua coleção"}},
    {"account.online",      {"Online", "En línea", "Online"}},
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

    // --- Estadísticas -------------------------------------------------------------------
    {"stats.title",         {"Your Stats", "Tus estadísticas", "Suas estatísticas"}},
    {"stats.playtime",      {"Total playtime", "Tiempo jugado", "Tempo de jogo"}},
    {"stats.sessions",      {"Sessions", "Sesiones", "Sessões"}},
    {"stats.hint",          {"Counted while Luma is loaded", "Se cuenta mientras Luma está cargado",
                             "Contado enquanto o Luma está carregado"}},
    {"stats.hours",         {"%d h %d min", "%d h %d min", "%d h %d min"}},
    {"stats.minutes",       {"%d min", "%d min", "%d min"}},

    // --- Noticias ---------------------------------------------------------------------------
    {"news.title",          {"Latest News", "Últimas noticias", "Últimas notícias"}},
    {"news.launcher_title", {"A brand new launcher", "Un launcher nuevo", "Um launcher novo"}},
    {"news.launcher_body",  {"Dark and light themes, one-click PLAY and three languages.",
                             "Temas oscuro y claro, JUGAR con un clic y tres idiomas.",
                             "Temas escuro e claro, JOGAR com um clique e três idiomas."}},
    {"news.world_title",    {"Hitboxes and block outline", "Hitboxes y contorno de bloque", "Hitboxes e contorno de bloco"}},
    {"news.world_body",     {"Drawn inside the world: walls hide them, like the game's own.",
                             "Dibujados dentro del mundo: las paredes los tapan, como los del juego.",
                             "Desenhados dentro do mundo: as paredes os escondem, como os do jogo."}},
    {"news.hud_title",      {"Resize from the corners", "Cambia el tamaño desde las esquinas",
                             "Redimensione pelos cantos"}},
    {"news.hud_body",       {"Drag a corner in the HUD editor, plus a movable paper doll.",
                             "Arrastra una esquina en el editor de HUD, y un muñeco movible.",
                             "Arraste um canto no editor de HUD, e um boneco móvel."}},

    // --- Ajustes ------------------------------------------------------------------------------
    {"settings.title",      {"Settings", "Ajustes", "Configurações"}},
    {"settings.subtitle",   {"Language, Luma keys and files.", "Idioma, teclas de Luma y archivos.", "Idioma, teclas do Luma e arquivos."}},
    {"settings.language_hint", {"The launcher uses the same language.", "El launcher usa el mismo idioma.", "O launcher usa o mesmo idioma."}},
    {"settings.general",    {"General", "General", "Geral"}},
    {"settings.language",   {"Language", "Idioma", "Idioma"}},
    {"settings.theme",      {"Theme", "Tema", "Tema"}},
    {"settings.dark",       {"Dark", "Oscuro", "Escuro"}},
    {"settings.light",      {"Light", "Claro", "Claro"}},
    {"settings.after_play", {"After PLAY", "Después de JUGAR", "Depois de JOGAR"}},
    {"settings.minimize",   {"Minimize", "Minimizar", "Minimizar"}},
    {"settings.stay",       {"Keep open", "Dejar abierto", "Manter aberto"}},
    {"settings.startup",    {"Open with Windows", "Abrir con Windows", "Abrir com o Windows"}},
    {"settings.startup_hint", {"Starts minimized when you turn on the PC.", "Se abre minimizado al encender la PC.",
                               "Abre minimizado ao ligar o PC."}},
    {"settings.lang_hint", {"Luma's in-game menu uses this language too.", "El menú de Luma en el juego también usa este idioma.", "O menu do Luma no jogo também usa este idioma."}},
    {"settings.account",    {"Account", "Cuenta", "Conta"}},
    {"settings.folders",    {"Folders", "Carpetas", "Pastas"}},
    {"settings.logs",       {"Logs", "Logs", "Logs"}},
    {"settings.config",     {"Config", "Config", "Config"}},
    {"settings.benchmarks", {"Benchmarks", "Benchmarks", "Benchmarks"}},
    {"settings.data",       {"All Luma data", "Todos los datos", "Todos os dados"}},
    {"settings.about",      {"About", "Acerca de", "Sobre"}},
    {"settings.about_text", {"Quality-of-life client for Minecraft Bedrock (Windows, GDK).",
                             "Client de calidad de vida para Minecraft Bedrock (Windows, GDK).",
                             "Client de qualidade de vida para Minecraft Bedrock (Windows, GDK)."}},
    {"settings.version",    {"Version %s", "Versión %s", "Versão %s"}},

    // --- Cuenta ---------------------------------------------------------------------------------
    {"account.sign_in",     {"Sign in with Microsoft", "Iniciar sesión con Microsoft", "Entrar com a Microsoft"}},
    {"account.sign_out",    {"Sign out", "Cerrar sesión", "Sair"}},
    {"account.cancel",      {"Cancel", "Cancelar", "Cancelar"}},
    {"account.why",         {"Optional: shows your Xbox gamertag and picture. Your password is only typed on Microsoft's page.",
                             "Opcional: muestra tu gamertag y tu foto de Xbox. La contraseña solo se escribe en la página de Microsoft.",
                             "Opcional: mostra seu gamertag e sua foto do Xbox. A senha só é digitada na página da Microsoft."}},
    {"account.waiting",     {"Finish signing in in your browser...", "Termina de iniciar sesión en el navegador...",
                             "Termine de entrar no navegador..."}},
    {"account.refreshing",  {"Restoring your session...", "Recuperando tu sesión...", "Restaurando sua sessão..."}},
    {"account.signed_in",   {"Signed in with Xbox", "Sesión iniciada con Xbox", "Conectado com o Xbox"}},
    {"account.err_network", {"Could not reach Microsoft. Check your internet connection.",
                             "No se pudo conectar con Microsoft. Revisa tu conexión a internet.",
                             "Não foi possível conectar à Microsoft. Verifique sua internet."}},
    {"account.err_timeout", {"The sign-in took too long. Try again.", "El inicio de sesión tardó demasiado. Prueba otra vez.",
                             "O login demorou demais. Tente de novo."}},
    {"account.err_no_xbox", {"This Microsoft account has no Xbox profile yet. Open Minecraft or xbox.com once to create it.",
                             "Esta cuenta de Microsoft todavía no tiene perfil de Xbox. Abre Minecraft o xbox.com una vez para crearlo.",
                             "Esta conta Microsoft ainda não tem perfil do Xbox. Abra o Minecraft ou xbox.com uma vez para criá-lo."}},
    {"account.err_child",   {"This is a child account: an adult has to allow it in the Xbox family settings.",
                             "Es una cuenta de menor: un adulto tiene que permitirlo en la configuración familiar de Xbox.",
                             "É uma conta infantil: um adulto precisa permitir nas configurações de família do Xbox."}},
    {"account.err_generic", {"Signing in failed. Try again.", "No se pudo iniciar sesión. Prueba otra vez.",
                             "Não foi possível entrar. Tente de novo."}},

    // --- Actualizaciones -----------------------------------------------------------------
    {"settings.updates",    {"Updates", "Actualizaciones", "Atualizações"}},
    {"update.checking",     {"Checking for updates...", "Buscando actualizaciones...", "Procurando atualizações..."}},
    {"update.up_to_date",   {"Luma is up to date", "Luma está al día", "O Luma está atualizado"}},
    {"update.available",    {"Update available: v%s", "Actualización disponible: v%s", "Atualização disponível: v%s"}},
    {"update.downloading",  {"Updating Luma to v%s...", "Actualizando Luma a v%s...", "Atualizando o Luma para v%s..."}},
    {"update.installing",   {"Installing the update...", "Instalando la actualización...", "Instalando a atualização..."}},
    {"update.updated_to",   {"Updated to v%s", "Actualizado a v%s", "Atualizado para v%s"}},
    {"update.offline",      {"Could not check for updates", "No se pudo buscar actualizaciones",
                             "Não foi possível procurar atualizações"}},
    {"update.failed",       {"The update failed. You keep the current version.",
                             "La actualización falló. Sigues con la versión actual.",
                             "A atualização falhou. Você continua com a versão atual."}},
    {"update.download_failed", {"The update could not be downloaded.", "No se pudo descargar la actualización.",
                                "Não foi possível baixar a atualização."}},
    {"update.bad_signature", {"The update did not pass the security check and was not installed.",
                              "La actualización no pasó la comprobación de seguridad y no se instaló.",
                              "A atualização não passou na verificação de segurança e não foi instalada."}},
    {"update.no_permission", {"Luma cannot write in its folder. Move LumaLauncher.exe to another folder (for example, Documents).",
                              "Luma no puede escribir en su carpeta. Mueve LumaLauncher.exe a otra (por ejemplo, Documentos).",
                              "O Luma não pode escrever na pasta dele. Mova o LumaLauncher.exe para outra (por exemplo, Documentos)."}},
    {"update.restart_failed", {"Updated: open Luma again to use the new version.",
                               "Actualizado: abre Luma de nuevo para usar la versión nueva.",
                               "Atualizado: abra o Luma de novo para usar a versão nova."}},
    {"update.dev_build",    {"development build", "compilación local", "compilação local"}},
    {"update.auto",         {"Update automatically", "Actualizar solo", "Atualizar sozinho"}},
    {"update.check_now",    {"Check now", "Buscar ahora", "Procurar agora"}},
    {"update.install_now",  {"Update now", "Actualizar ahora", "Atualizar agora"}},
    {"play.unsupported",    {"Minecraft %s is not supported by this Luma yet. Some modules may not work.",
                             "Minecraft %s todavía no está soportado por esta Luma. Algunos módulos pueden no funcionar.",
                             "O Minecraft %s ainda não é suportado por este Luma. Alguns módulos podem não funcionar."}},
    {"play.anyway",         {"Play anyway", "Jugar igual", "Jogar mesmo assim"}},

    // --- Menú del juego: radial y Closet -------------------------------------------------
    {"tab.local",           {"Local world", "Mundo local", "Mundo local"}},
    {"tab.server",          {"Server", "Servidor", "Servidor"}},
    {"tab.players",         {"%d players", "%d jugadores", "%d jogadores"}},
    {"tab.players_ping",    {"%d players  ·  %d ms", "%d jugadores  ·  %d ms", "%d jogadores  ·  %d ms"}},
    {"radial.mods",         {"Mods", "Mods", "Mods"}},
    {"radial.hud",          {"HUD", "HUD", "HUD"}},
    {"radial.closet",       {"Closet", "Closet", "Closet"}},
    {"radial.settings",     {"Settings", "Ajustes", "Configurações"}},
    {"radial.home",         {"Home", "Inicio", "Início"}},
    {"radial.hint",         {"%s or Esc to close", "%s o Esc para cerrar", "%s ou Esc para fechar"}},
    {"closet.title",        {"Closet", "Closet", "Closet"}},
    {"closet.subtitle",     {"Your skins and, soon, Luma cosmetics.", "Tus skins y, pronto, los cosméticos de Luma.",
                             "Suas skins e, em breve, os cosméticos do Luma."}},
    {"closet.skins",        {"Your skins", "Tus skins", "Suas skins"}},
    {"closet.empty",        {"No skins yet: they are saved here on their own when you play, or import them from the launcher.",
                             "Todavía no hay skins: se guardan solas al jugar, o impórtalas desde el launcher.",
                             "Ainda não há skins: elas são salvas sozinhas ao jogar, ou importe pelo launcher."}},
    {"closet.equip",        {"Equip", "Equipar", "Equipar"}},
    {"closet.equipped",     {"Equipped", "Equipada", "Equipada"}},
    {"closet.equipped_toast", {"%s is ready: pick it in the Dressing Room, \"Luma\" pack", "%s lista: elígela en el Vestidor, paquete \"Luma\"", "%s pronta: escolha no Provador, pacote \"Luma\""}},
    {"closet.equip_hint",   {"The equipped skin is the one Luma uses (cosmetics, launcher). Changing your real Minecraft skin from here is coming later.",
                             "La skin equipada es la que usa Luma (cosméticos, launcher). Cambiar tu skin real de Minecraft desde aquí llega más adelante.",
                             "A skin equipada é a que o Luma usa (cosméticos, launcher). Trocar sua skin real do Minecraft por aqui chega mais tarde."}},
    {"closet.cosmetics",    {"Cosmetics", "Cosméticos", "Cosméticos"}},
    {"closet.cosmetics_soon", {"Capes and more, visible between Luma users. Coming soon.",
                               "Capas y más, visibles entre usuarios de Luma. Próximamente.",
                               "Capas e mais, visíveis entre usuários do Luma. Em breve."}},

    // --- Secciones que aún no existen ---------------------------------------------
    // --- Consejos de la barra lateral ---
    {"tip.title",           {"Tip", "Consejo", "Dica"}},
    {"tip.menu",            {"Press L in the game to open Luma.", "Pulsa L en el juego para abrir Luma.", "Aperte L no jogo para abrir o Luma."}},
    {"tip.tab",             {"Hold Tab to see the player list with faces and your ping.", "Mantén Tab para ver la lista de jugadores con sus caras y tu ping.", "Segure Tab para ver a lista de jogadores com rostos e seu ping."}},
    {"tip.unload",          {"Press End to unload Luma without closing the game.", "Pulsa Fin para descargar Luma sin cerrar el juego.", "Aperte End para descarregar o Luma sem fechar o jogo."}},
    {"tip.skins",           {"Your skins are saved here by themselves every time you play.", "Tus skins se guardan aquí solas cada vez que juegas.", "Suas skins são salvas aqui sozinhas sempre que você joga."}},
    {"tip.view",            {"In Luma's menu you can show modules as blocks or as a list.", "En el menú de Luma puedes ver los módulos en bloques o en lista.", "No menu do Luma você pode ver os módulos em blocos ou em lista."}},
    {"tip.zoom",            {"Hold C to zoom, and use the wheel to zoom in further.", "Mantén C para hacer zoom, y usa la rueda para acercar más.", "Segure C para dar zoom, e use a roda para aproximar mais."}},
    {"tip.language",        {"Luma speaks English, Spanish and Portuguese: change it in Settings.", "Luma habla inglés, español y portugués: cámbialo en Ajustes.", "O Luma fala inglês, espanhol e português: mude em Configurações."}},
    // --- Add-ons ---
    {"addons.subtitle",     {"The packs installed in your Minecraft", "Los paquetes instalados en tu Minecraft", "Os pacotes instalados no seu Minecraft"}},
    {"addons.open",         {"Open folder", "Abrir carpeta", "Abrir pasta"}},
    {"addons.refresh",      {"Refresh", "Actualizar", "Atualizar"}},
    {"addons.resource",     {"Resource packs", "Paquetes de recursos", "Pacotes de recursos"}},
    {"addons.behavior",     {"Behavior packs", "Paquetes de comportamiento", "Pacotes de comportamento"}},
    {"addons.skin",         {"Skin packs", "Paquetes de skins", "Pacotes de skins"}},
    {"addons.installed",    {"Installed", "Instalados", "Instalados"}},
    {"addons.empty",        {"No packs installed yet.", "Todavía no hay paquetes instalados.", "Ainda não há pacotes instalados."}},
    {"addons.kind_resource", {"Resources", "Recursos", "Recursos"}},
    {"addons.kind_behavior", {"Behavior", "Comportamiento", "Comportamento"}},
    {"addons.kind_skin",    {"Skins", "Skins", "Skins"}},
    {"addons.soon1",        {"Turn packs on and off with one click.", "Activar y desactivar paquetes con un clic.", "Ativar e desativar pacotes com um clique."}},
    {"addons.soon2",        {"Import .mcpack and .mcaddon files by dragging them here.", "Importar archivos .mcpack y .mcaddon arrastrándolos aquí.", "Importar arquivos .mcpack e .mcaddon arrastando aqui."}},
    {"addons.soon3",        {"Pack previews with their icon and description.", "Vista previa de cada paquete con su ícono y descripción.", "Prévia de cada pacote com ícone e descrição."}},
    {"addons.soon4",        {"Recommended packs that pair well with Luma.", "Paquetes recomendados que combinan bien con Luma.", "Pacotes recomendados que combinam bem com o Luma."}},
    // --- Skins: guía ---
    {"skins.guide_title",   {"How to wear it in Minecraft", "Cómo ponértela en Minecraft", "Como usar no Minecraft"}},
    {"skins.guide1",        {"Press Equip: it goes first in the \"Luma\" skin pack.", "Pulsa Equipar: queda primera en el paquete \"Luma\".", "Aperte Equipar: ela fica primeiro no pacote \"Luma\"."}},
    {"skins.guide2",        {"Restart Minecraft (it reads skin packs when it opens).", "Reinicia Minecraft (lee los paquetes al abrirse).", "Reinicie o Minecraft (ele lê os pacotes ao abrir)."}},
    {"skins.guide3",        {"Dressing Room > Classic skins > \"Luma\" pack, and pick it.", "Vestidor > Skins clásicas > paquete \"Luma\", y elígela.", "Provador > Skins clássicas > pacote \"Luma\", e escolha."}},
    // --- Ajustes: atajos y enlaces ---
    {"settings.shortcuts",  {"Luma shortcuts", "Atajos de Luma", "Atalhos do Luma"}},
    {"shortcut.end_key",    {"End", "Fin", "End"}},
    {"shortcut.menu",       {"Open Luma's menu", "Abrir el menú de Luma", "Abrir o menu do Luma"}},
    {"shortcut.unload",     {"Unload Luma", "Descargar Luma", "Descarregar o Luma"}},
    {"shortcut.tab",        {"Player list", "Lista de jugadores", "Lista de jogadores"}},
    {"shortcut.zoom",       {"Zoom", "Zoom", "Zoom"}},
    {"settings.links",      {"Links", "Enlaces", "Links"}},
    {"settings.report",     {"Report a problem", "Reportar un problema", "Relatar um problema"}},
    {"settings.license",    {"Luma is free software under the GPL-3.0 license.", "Luma es software libre con licencia GPL-3.0.", "O Luma é software livre com licença GPL-3.0."}},
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
