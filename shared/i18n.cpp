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
    {"nav.lyrics", {"Lyrics", "Letras", "Letras"}},
    {"nav.spotify", {"Spotify", "Spotify", "Spotify"}},
    {"home.good_morning", {"Good morning", "Buenos días", "Bom dia"}},
    {"home.good_afternoon", {"Good afternoon", "Buenas tardes", "Boa tarde"}},
    {"home.good_evening", {"Good evening", "Buenas noches", "Boa noite"}},
    {"home.good_night", {"Late night", "Buenas noches", "Boa madrugada"}},
    {"home.music_tagline", {"Your music, inside Minecraft.", "Tu música, dentro de Minecraft.", "Sua música, dentro do Minecraft."}},
    {"music.now_playing", {"Now playing", "Sonando ahora", "Tocando agora"}},
    {"music.paused", {"Paused", "En pausa", "Pausado"}},
    {"music.nothing_title", {"Play some music", "Pon algo de música", "Coloque uma música"}},
    {"music.nothing_text", {"Spotify, YouTube Music, a browser... any app with media controls shows up here, and in the game.", "Spotify, YouTube Music, un navegador... cualquier app con controles de música aparece aquí, y en el juego.", "Spotify, YouTube Music, um navegador... qualquer app com controles de música aparece aqui, e no jogo."}},
    {"lyrics.soon_text", {"The lyrics' styles, with their previews, come next.", "Los estilos de las letras, con su vista previa, vienen en la próxima parte.", "Os estilos das letras, com a prévia, vêm na próxima parte."}},
    {"spotify.soon_text", {"Connecting Spotify and your queue come in a next part.", "Conectar Spotify y tu cola vienen en una próxima parte.", "Conectar o Spotify e sua fila vêm numa próxima parte."}},
    {"orb.in_game", {"In the game", "En el juego", "No jogo"}},
    {"lyrics.title", {"Lyrics in the sky", "Letras en el cielo", "Letras no céu"}},
    {"lyrics.subtitle", {"The song's lyrics floating in your Minecraft world. Pick a look: it's saved at once and the game uses it.", "La letra de la canción flotando en tu mundo de Minecraft. Elige cómo se ve: se guarda al instante y el juego la usa.", "A letra da música flutuando no seu mundo de Minecraft. Escolha o visual: é salvo na hora e o jogo usa."}},
    {"lyrics.on", {"Show them", "Mostrarlas", "Mostrar"}},
    {"lyrics.where_world", {"In the world", "En el mundo", "No mundo"}},
    {"lyrics.where_sky", {"In the sky", "En el cielo", "No céu"}},
    {"lyrics.live", {"Changes live in the game", "Cambia en vivo en el juego", "Muda ao vivo no jogo"}},
    {"lyrics.custom", {"Your own style (changed in the game): save it so you don't lose it.", "Tu propio estilo (cambiado en el juego): guárdalo para no perderlo.", "Seu próprio estilo (mudado no jogo): salve para não perder."}},
    {"lyrics.more", {"More options (size, distance, font...) are in the game: Luma's menu > Music > Lyric Flow.", "Más opciones (tamaño, distancia, fuente...) están en el juego: menú de Luma > Música > Lyric Flow.", "Mais opções (tamanho, distância, fonte...) estão no jogo: menu do Luma > Música > Lyric Flow."}},
    {"lyrics.no_config", {"Luma's settings file is damaged: open the game once to repair it.", "El archivo de ajustes de Luma está dañado: abre el juego una vez para repararlo.", "O arquivo de configurações do Luma está danificado: abra o jogo uma vez para repará-lo."}},
    {"lyrics.module_off", {"The \"Now playing\" module is off in the game: turn it on to see them.", "El módulo \"Sonando ahora\" está apagado en el juego: actívalo para verlas.", "O módulo \"Tocando agora\" está desligado no jogo: ligue-o para vê-las."}},
    {"lyrics.searching", {"Looking for the lyrics...", "Buscando la letra...", "Procurando a letra..."}},
    {"lyrics.not_found", {"No lyrics found for this song", "No encontramos la letra de esta canción", "Não achamos a letra desta música"}},
    {"lyrics.instrumental", {"Instrumental", "Instrumental", "Instrumental"}},
    {"lyrics.unsynced", {"These lyrics have no timings", "Esta letra no tiene tiempos", "Esta letra não tem tempos"}},
    {"style.karaoke", {"Karaoke", "Karaoke", "Karaokê"}},
    {"style.karaoke_d", {"The album's color, filling as it's sung", "El color del álbum, llenándose al cantar", "A cor do álbum, enchendo ao cantar"}},
    {"style.neon", {"Neon", "Neón", "Neon"}},
    {"style.neon_d", {"Bright cyan, a neon glow and sparks", "Cian brillante, brillo neón y chispas", "Ciano brilhante, brilho neon e faíscas"}},
    {"style.party", {"Party", "Fiesta", "Festa"}},
    {"style.party_d", {"A rainbow, confetti and a glow that pulses", "Arcoíris, confeti y un brillo que late", "Arco-íris, confete e um brilho que pulsa"}},
    {"style.dream", {"Dream", "Sueño", "Sonho"}},
    {"style.dream_d", {"The album's gradient, soft light, stardust", "Degradado del álbum, luz suave, polvo de estrellas", "Degradê do álbum, luz suave, poeira estelar"}},
    {"style.fire", {"Fire", "Fuego", "Fogo"}},
    {"style.fire_d", {"Orange to yellow, embers, 3D letters", "Naranja a amarillo, brasas, letras 3D", "Laranja ao amarelo, brasas, letras 3D"}},
    {"style.minimal", {"Minimal", "Mínimo", "Mínimo"}},
    {"style.minimal_d", {"White with a border, nothing else", "Blanco con borde, nada más", "Branco com borda, nada mais"}},
    {"opt.offset", {"Lyrics timing", "Tiempo de la letra", "Tempo da letra"}},
    {"opt.copy", {"Copy the song's name", "Copiar el nombre", "Copiar o nome"}},
    {"opt.copied", {"Copied", "Copiado", "Copiado"}},
    {"opt.open_spotify", {"Open Spotify", "Abrir Spotify", "Abrir o Spotify"}},
    {"lyrics.your_styles", {"Your styles", "Tus estilos", "Seus estilos"}},
    {"lyrics.ready_styles", {"Ready-made styles", "Estilos listos", "Estilos prontos"}},
    {"lyrics.save_style", {"Save this style", "Guardar este estilo", "Salvar este estilo"}},
    {"lyrics.my_style", {"My style", "Mi estilo", "Meu estilo"}},
    {"lyrics.backup", {"Your previous style", "Tu estilo anterior", "Seu estilo anterior"}},
    {"lyrics.delete_confirm", {"Delete?", "¿Borrar?", "Apagar?"}},
    {"lyrics.no_styles", {"Save how your lyrics look so you never lose it.", "Guarda cómo se ven tus letras para no perderlo nunca.", "Salve como suas letras ficam para nunca perder."}},
    {"lyrics.update_style", {"Update \"%s\"", "Actualizar \"%s\"", "Atualizar \"%s\""}},
    {"lyrics.save_new", {"Save as new", "Guardar como nuevo", "Salvar como novo"}},
    {"lyrics.styles_hint", {"Click to use it. Double click to rename it. The x deletes it (click it twice).", "Clic para usarlo. Doble clic para cambiarle el nombre. La x lo borra (dos clics).", "Clique para usar. Clique duas vezes para renomear. O x apaga (dois cliques)."}},
    {"key.play", {"Playing", "Reproduciendo", "Tocando"}},
    {"key.previous", {"Previous song", "Canción anterior", "Música anterior"}},
    {"key.next", {"Next song", "Siguiente canción", "Próxima música"}},
    {"key.volume", {"Volume %d%%", "Volumen %d%%", "Volume %d%%"}},
    {"key.help", {"Keys: Space play / pause  ·  Left / Right 5 seconds  ·  Ctrl + Left / Right song  ·  Up / Down volume", "Teclas: Espacio pausa  ·  Izquierda / Derecha 5 segundos  ·  Ctrl + Izquierda / Derecha canción  ·  Arriba / Abajo volumen", "Teclas: Espaço pausa  ·  Esquerda / Direita 5 segundos  ·  Ctrl + Esquerda / Direita música  ·  Cima / Baixo volume"}},
    {"spotify.subtitle", {"Your Spotify, inside Luma: what's playing, your library and your playlists. It uses your own Spotify app, so it only talks to Spotify.", "Tu Spotify dentro de Luma: lo que suena, tu biblioteca y tus playlists. Usa tu propia app de Spotify, así que solo habla con Spotify.", "Seu Spotify dentro do Luma: o que toca, sua biblioteca e suas playlists. Usa seu próprio app do Spotify, então só fala com o Spotify."}},
    {"spotify.guide_title", {"Connect Spotify (once, 2 minutes)", "Conecta Spotify (una vez, 2 minutos)", "Conecte o Spotify (uma vez, 2 minutos)"}},
    {"spotify.guide_why", {"Spotify asks each program to have its own \"app\". You create yours (it's free) and Luma uses it: nothing goes through other servers.", "Spotify pide que cada programa tenga su propia \"app\". Creas la tuya (es gratis) y Luma la usa: nada pasa por otros servidores.", "O Spotify pede que cada programa tenha seu próprio \"app\". Você cria o seu (é grátis) e o Luma usa: nada passa por outros servidores."}},
    {"spotify.step1", {"Open the Spotify Developer dashboard and sign in with your Spotify account.", "Abre el panel de desarrolladores de Spotify e inicia sesión con tu cuenta de Spotify.", "Abra o painel de desenvolvedores do Spotify e entre com sua conta do Spotify."}},
    {"spotify.open_dashboard", {"Open the dashboard", "Abrir el panel", "Abrir o painel"}},
    {"spotify.step2", {"Press \"Create app\". Name: Luma (or what you like). Description: anything.", "Toca \"Create app\". Nombre: Luma (o el que quieras). Descripción: cualquiera.", "Toque \"Create app\". Nome: Luma (ou o que quiser). Descrição: qualquer uma."}},
    {"spotify.step3", {"In \"Redirect URIs\" paste exactly this address and press \"Add\":", "En \"Redirect URIs\" pega exactamente esta dirección y toca \"Add\":", "Em \"Redirect URIs\" cole exatamente este endereço e toque \"Add\":"}},
    {"spotify.copy", {"Copy", "Copiar", "Copiar"}},
    {"spotify.step4", {"Under \"Which API/SDKs are you planning to use?\" check \"Web API\", accept the terms and press \"Save\".", "En \"Which API/SDKs are you planning to use?\" marca \"Web API\", acepta los términos y toca \"Save\".", "Em \"Which API/SDKs are you planning to use?\" marque \"Web API\", aceite os termos e toque \"Save\"."}},
    {"spotify.step5", {"Copy your app's \"Client ID\" (on its page, in Basic Information) and paste it here:", "Copia el \"Client ID\" de tu app (en su página, en Basic Information) y pégalo aquí:", "Copie o \"Client ID\" do seu app (na página dele, em Basic Information) e cole aqui:"}},
    {"spotify.client_id_hint", {"32 letters and numbers", "32 letras y números", "32 letras e números"}},
    {"spotify.paste", {"Paste", "Pegar", "Colar"}},
    {"spotify.bad_id", {"That doesn't look like a Client ID (it has 32 letters and numbers).", "Eso no parece un Client ID (tiene 32 letras y números).", "Isso não parece um Client ID (tem 32 letras e números)."}},
    {"spotify.step6", {"Connect: Spotify's page opens in your browser. Accept there, and come back.", "Conecta: se abre la página de Spotify en tu navegador. Acepta ahí y vuelve.", "Conecte: a página do Spotify abre no navegador. Aceite lá e volte."}},
    {"spotify.connect", {"Connect with Spotify", "Conectar con Spotify", "Conectar com o Spotify"}},
    {"spotify.premium_note", {"Note: Spotify only lets Premium accounts control the music from other programs, and asks the app's owner to have Premium. Without it you'll see your music but the buttons won't work.", "Ojo: Spotify solo deja que las cuentas Premium controlen la música desde otros programas, y pide que el dueño de la app tenga Premium. Sin Premium verás tu música, pero los botones no funcionarán.", "Atenção: o Spotify só deixa contas Premium controlarem a música de outros programas, e pede que o dono do app tenha Premium. Sem Premium você verá sua música, mas os botões não vão funcionar."}},
    {"spotify.waiting", {"Waiting for you in the browser...", "Esperándote en el navegador...", "Esperando você no navegador..."}},
    {"spotify.cancel", {"Cancel", "Cancelar", "Cancelar"}},
    {"spotify.error", {"Couldn't connect.", "No se pudo conectar.", "Não foi possível conectar."}},
    {"spotify.connected", {"Connected", "Conectado", "Conectado"}},
    {"spotify.shared", {"Connected in the game too", "Conectado también en el juego", "Conectado no jogo também"}},
    {"spotify.disconnect", {"Disconnect", "Desconectar", "Desconectar"}},
    {"spotify.reconnect", {"Connect again", "Conectar de nuevo", "Conectar de novo"}},
    {"spotify.outdated", {"You connected with an older Luma, which only asked to see your queue. Connect again to give the new permissions (control the music, your library and playlists).", "Conectaste con un Luma más viejo, que solo pedía ver tu cola. Conecta de nuevo para dar los permisos nuevos (controlar la música, tu biblioteca y playlists).", "Você conectou com um Luma mais antigo, que só pedia ver sua fila. Conecte de novo para dar as novas permissões (controlar a música, sua biblioteca e playlists)."}},
    {"spotify.free_note", {"Your account is Free: Spotify doesn't let other programs control the music without Premium. You'll still see your library and playlists.", "Tu cuenta es Free: Spotify no deja que otros programas controlen la música sin Premium. Igual verás tu biblioteca y tus playlists.", "Sua conta é Free: o Spotify não deixa outros programas controlarem a música sem Premium. Você ainda verá sua biblioteca e playlists."}},
    {"spotify.next_part", {"All set. The player (controls, devices, your library) comes in the next part.", "Todo listo. El reproductor (controles, dispositivos, tu biblioteca) llega en la próxima parte.", "Tudo pronto. O player (controles, dispositivos, sua biblioteca) chega na próxima parte."}},
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
    {"radial.music",        {"Music", "Música", "Música"}},
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
    {"closet.capes",        {"Luma capes", "Capas de Luma", "Capas da Luma"}},
    {"closet.capes_hint",   {"Only you and other Luma users see it: your skin on the server doesn't change. Persona skins may not show capes.",
                             "Solo la ven tú y otros usuarios de Luma: tu skin en el servidor no cambia. Las skins Persona pueden no mostrar capas.",
                             "Só você e outros usuários do Luma a veem: sua skin no servidor não muda. Skins Persona podem não mostrar capas."}},
    {"closet.cape_none",    {"None", "Ninguna", "Nenhuma"}},
    {"closet.cape_classic", {"Classic", "Clásica", "Clássica"}},
    {"closet.cape_dawn",    {"Dawn", "Amanecer", "Amanhecer"}},
    {"closet.cape_night",   {"Night", "Noche", "Noite"}},
    {"closet.cape_grid",    {"Grid", "Cuadrícula", "Grade"}},
    {"closet.cape_galaxy",  {"Galaxy", "Galaxia", "Galáxia"}},
    {"closet.cape_apple",   {"Apple", "Manzana", "Maçã"}},
    {"closet.cape_wear",    {"Wear", "Usar", "Usar"}},
    {"closet.cape_wearing", {"Wearing", "Puesta", "Em uso"}},

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
