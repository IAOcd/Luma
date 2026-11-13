// =============================================================================
//  input.cpp - Nuestra WndProc y los hooks de input de Windows
// =============================================================================
//  Qué hace cada pieza (todas son funciones de Windows, estables entre
//  versiones del juego):
//
//    - WndProc propia: detecta la tecla del menú, Esc y las teclas de los
//      módulos; con el menú abierto le pasa el teclado y el mouse a ImGui y no
//      se los entrega al juego.
//    - PeekMessageW: el juego saca los mensajes de su cola con esta función y
//      a veces los mira ANTES de entregarlos a la ventana. Con el menú abierto,
//      Luma los procesa ahí y le devuelve al juego un mensaje vacío.
//    - GetAsyncKeyState / GetKeyState: con el menú abierto, el juego ve todas
//      las teclas sin apretar.
//    - ClipCursor: con el menú abierto, el juego no puede volver a encerrar el
//      cursor.
//
//  El mouse del JUEGO (cámara y clics) no se bloquea aquí: llega por una cola
//  interna del juego, y eso lo resuelven sdk/mouse_grab y sdk/mouse_queue.
//  Probamos muchas otras vías de Windows (raw input, GetCursorPos, GameInput);
//  la historia completa está en el README.
// =============================================================================
#include "render/input.h"
#include "render/hud.h"

#include <intrin.h>  // _ReturnAddress: desde dónde nos llamaron

#include <atomic>
#include <cstdint>
#include <cwctype>
#include <mutex>

#include "core/clicks.h"
#include "core/config.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "render/overlay.h"
#include "sdk/mouse_grab.h"
#include "sdk/mouse_queue.h"

namespace luma::render::input {

namespace {

// La tecla del menú (por defecto L) está en config::g_menuKey y se cambia
// desde Ajustes.
//
// LIMITACIÓN CONOCIDA: Luma todavía no sabe si el chat del juego está
// abierto, así que si la tecla del menú es una letra, escribirla en el chat
// abre el menú. Se corrige en la Fase 7, cuando Luma pueda leer ese estado
// del juego.

HWND g_window = nullptr;
// La WndProc original del juego. Atómica porque la leen otros hilos.
std::atomic<WNDPROC> g_originalWndProc{nullptr};
std::atomic<bool> g_hooksActive{false};

// Tras una tecla que usó Luma (por ejemplo, abrir el menú con L), Windows
// genera además un WM_CHAR con el carácter ('l'). Lo descartamos para que
// no se escriba en el chat del juego. 0 = no descartar nada.
wchar_t g_swallowChar = 0;

// --- Funciones originales de Windows (los "trampolines" de MinHook) -----------------
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using PeekMessageWFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using KeyStateFn = SHORT(WINAPI*)(int);
ClipCursorFn g_originalClipCursor = nullptr;
PeekMessageWFn g_originalPeekMessageW = nullptr;
KeyStateFn g_originalGetAsyncKeyState = nullptr;
KeyStateFn g_originalGetKeyState = nullptr;

// Rango de memoria donde está cargado Luma.dll. Sirve para saber si quien
// llama a GetKeyState es Luma (ImGui necesita el estado real) o el juego.
// "__ImageBase" es un símbolo que el enlazador de Microsoft define al inicio
// de nuestro propio módulo.
extern "C" IMAGE_DOS_HEADER __ImageBase;
std::uintptr_t g_lumaStart = 0;
std::uintptr_t g_lumaEnd = 0;

bool IsLumaAddress(void* address) {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= g_lumaStart && value < g_lumaEnd;
}

// Último encierro de cursor que pidió el juego (para restaurarlo si el
// camino principal, sdk/mouse_grab, no estuviera disponible).
std::mutex g_clipMutex;
bool g_gameClipKnown = false;
bool g_gameClipActive = false;  // false = el juego pidió "sin encierro"
RECT g_gameClipRect{};

// --- Teclas sostenidas (ver input.h) ----------------------------------------------
// Un mensaje propio de Luma: "sincroniza las teclas sostenidas". Se lo
// mandamos a la ventana con PostMessage para que el trabajo ocurra en el hilo
// de la ventana (el único donde se puede llamar a la WndProc del juego).
constexpr UINT kHoldSyncMessage = WM_APP + 0x4C4D;
constexpr UINT kToggleMenuMessage = WM_APP + 0x4C4E;
constexpr UINT kTapKeyMessage = WM_APP + 0x4C4F;
constexpr UINT kFreecamReleaseMessage = WM_APP + 0x4C50;

struct Hold {
    std::atomic<bool> enabled{false};   // el módulo está activo
    std::atomic<int> vk{0};             // la tecla del juego
    std::atomic<bool> active{true};     // estado del "toggle" (pulsar la tecla lo alterna)
    std::atomic<bool> applied{false};   // ¿el juego la cree apretada por nosotros?
    int appliedVk = 0;                  // con qué tecla la apretamos (hilo de la ventana)
};
Hold g_holds[2];
std::atomic<bool> g_holdsSuspended{false};  // en un menú del juego o sin foco
std::atomic<bool> g_dropPaused{false};      // Ctrl let go for a moment: you're dropping an item (Q)
std::atomic<bool> g_wheelCapture{false};
// Free cam: las teclas de movimiento son de la cámara, no del jugador.
std::atomic<bool> g_freecamKeys{false};
constexpr int kFreecamKeys[] = {'W', 'A', 'S', 'D', VK_SPACE, VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
                                VK_CONTROL, VK_LCONTROL, VK_RCONTROL};
bool IsFreecamKey(int vk) {
    for (int k : kFreecamKeys) {
        if (k == vk) return true;
    }
    return false;
}
std::atomic<int> g_wheelDelta{0};  // en unidades de WHEEL_DELTA (120 = un paso)

// ¿La tecla "vk" que preguntan es la sostenida? Ctrl, Shift y Alt tienen
// además versión izquierda (VK_LCONTROL...), que el juego podría usar.
bool SameKey(int asked, int held) {
    if (asked == held) {
        return true;
    }
    switch (held) {
        case VK_CONTROL: return asked == VK_LCONTROL;
        case VK_SHIFT:   return asked == VK_LSHIFT;
        case VK_MENU:    return asked == VK_LMENU;
        default:         return false;
    }
}

// --- Utilidades ---------------------------------------------------------------------

// El carácter que produce una tecla (en minúscula), o 0 si no produce ninguno.
wchar_t CharForKey(int vk) {
    const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFF;
    return static_cast<wchar_t>(towlower(static_cast<wint_t>(ch)));
}

// Convierte un mensaje de botón extra del mouse en su "tecla" virtual, para
// poder asignar Mouse 4 / Mouse 5 / clic central a los módulos.
int MouseButtonKey(UINT message, WPARAM wParam) {
    if (message == WM_MBUTTONDOWN) {
        return VK_MBUTTON;
    }
    if (message == WM_XBUTTONDOWN) {
        return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    }
    return 0;
}

// ¿Es un botón del mouse? (VK_LBUTTON=1, VK_RBUTTON=2, VK_MBUTTON=4, X1=5, X2=6)
bool IsMouseButtonKey(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
           vk == VK_XBUTTON2;
}

// Mensajes de "puntero" (WM_POINTERDOWN, WM_POINTERUPDATE...): la forma
// moderna en que Windows avisa de mouse, lápiz y pantalla táctil. Van del
// 0x0245 al 0x0257 y NO están dentro de WM_MOUSEFIRST..WM_MOUSELAST.
bool IsPointerMessage(UINT message) {
    return message >= 0x0245 && message <= 0x0257;
}

// ¿Es un mensaje de teclado o de mouse? (Los que bloqueamos con el menú abierto.)
bool IsInputMessage(UINT message) {
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
           (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT ||
           IsPointerMessage(message);
}

// ¿Es un mensaje de "soltar" una tecla o un botón del mouse?
bool IsReleaseMessage(UINT message) {
    return message == WM_KEYUP || message == WM_SYSKEYUP || message == WM_LBUTTONUP ||
           message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

// Llama a la ClipCursor real (sin pasar por nuestro detour).
BOOL RealClipCursor(const RECT* rect) {
    return g_originalClipCursor != nullptr ? g_originalClipCursor(rect) : ::ClipCursor(rect);
}

void RestoreGameClip() {
    std::lock_guard lock(g_clipMutex);
    if (g_gameClipKnown) {
        RealClipCursor(g_gameClipActive ? &g_gameClipRect : nullptr);
    }
}

// Le "aprieta" o "suelta" una tecla al juego: le pasamos a su WndProc el
// mismo mensaje que mandaría Windows. Solo en el hilo de la ventana.
void SendKeyToGame(HWND window, int vk, bool down) {
    WNDPROC original = g_originalWndProc.load();
    if (window == nullptr || original == nullptr || vk == 0) {
        return;
    }
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    // lParam: repetición 1 y código de escaneo; al soltar, bits 30 y 31.
    const LPARAM lParam = static_cast<LPARAM>((down ? 0x00000001u : 0xC0000001u) | (scan << 16));
    CallWindowProcW(original, window, down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), lParam);
}

// Pone cada tecla sostenida como debe estar. En el hilo de la ventana.
void SyncHolds(HWND window) {
    const bool suspended = g_holdsSuspended.load();
    for (Hold& h : g_holds) {
        const int vk = h.vk.load();
        const bool want = h.enabled.load() && h.active.load() && !suspended && vk != 0;
        if (h.applied.load() && (!want || h.appliedVk != vk)) {
            SendKeyToGame(window, h.appliedVk, false);
            h.applied.store(false);
        }
        if (want && !h.applied.load()) {
            SendKeyToGame(window, vk, true);
            h.appliedVk = vk;
            h.applied.store(true);
        }
    }
}

// Una tecla real llegó a la ventana. Devuelve true si el juego NO debe verla.
bool HandleHoldKey(UINT message, WPARAM wParam, LPARAM lParam) {
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if ((!down && !up) || g_holdsSuspended.load()) {
        return false;
    }
    const int vk = static_cast<int>(wParam);
    for (Hold& h : g_holds) {
        if (!h.enabled.load() || h.vk.load() != vk) {
            continue;
        }
        if (down && (lParam & (1 << 30)) == 0) {
            // Pulsaste la tecla: alterna. Este WM_KEYDOWN le llega al juego igual.
            const bool nowActive = !h.active.load();
            h.active.store(nowActive);
            // Si queda activa, la tecla "es nuestra" (nos guardamos el soltar).
            // Si no, el soltar real le llega al juego y la suelta.
            h.applied.store(nowActive);
            h.appliedVk = vk;
            return false;
        }
        if (up && h.applied.load()) {
            return true;  // sigue "apretada" para el juego
        }
    }
    return false;
}

void PostHoldSync() {
    if (g_window != nullptr) {
        PostMessageW(g_window, kHoldSyncMessage, 0, 0);
    }
}

// ¿Estás jugando? (no en el chat, un cartel, el inventario o la pausa: ahí
// el juego suelta el mouse). Si no se sabe (firmas no encontradas), sí.
bool PlayingInGame() {
    return sdk::mouse::GameMouseState() != 0;
}

// --- WndProc --------------------------------------------------------------------

LRESULT CALLBACK LumaWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Contamos esta llamada como "activa" para que la descarga espere a que
    // termine (ver hooks.h).
    hooks::ActiveCallGuard guard;

    // --- Teclas ------------------------------------------------------------------
    // WM_SYSKEYDOWN es lo mismo que WM_KEYDOWN pero para Alt, F10 y
    // combinaciones con Alt.
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const int mouseKey = MouseButtonKey(message, wParam);
    if (keyDown || mouseKey != 0) {
        const int vk = keyDown ? static_cast<int>(wParam) : mouseKey;
        // El bit 30 de lParam indica "la tecla ya estaba apretada" (repetición
        // automática al mantenerla): la ignoramos para que nada parpadee.
        const bool isRepeat = keyDown && (lParam & (1 << 30)) != 0;

        // 1. El menú está esperando una tecla para asignarla.
        if (overlay::IsCapturingKey()) {
            if (!isRepeat) {
                overlay::OnKeyDown(vk);
            }
            g_swallowChar = keyDown ? CharForKey(vk) : 0;
            return 0;
        }
        // 2. Esc cierra el menú. El juego no lo ve (si no, abriría la pausa).
        if (vk == VK_ESCAPE && overlay::IsMenuOpen()) {
            // Si está el editor de HUD, Esc vuelve al menú en vez de cerrarlo.
            if (!isRepeat && !overlay::HandleEscape()) {
                overlay::ToggleMenu();
            }
            return 0;
        }
        // F1 (the game hides its HUD): render/hud.cpp reads that key by itself.
        // 3. La tecla del menú. Si ImGui espera texto (la caja de búsqueda),
        //    la letra se escribe en vez de cerrar el menú.
        // Solo jugando o para CERRAR el menú: si escribes en el chat, un
        //    cartel o el inventario, la letra es para el juego.
        if (vk == config::g_menuKey.load(std::memory_order_relaxed) && !overlay::WantsTextInput() &&
            (overlay::IsMenuOpen() || PlayingInGame())) {
            if (!isRepeat) {
                overlay::ToggleMenu();
            }
            g_swallowChar = keyDown ? CharForKey(vk) : 0;
            return 0;  // El juego no ve esta tecla.
        }
        // 4. Teclas de los módulos (el juego también las recibe).
        //    También solo jugando o con el menú de Luma abierto.
        if (!isRepeat && !overlay::WantsTextInput() && (overlay::IsMenuOpen() || PlayingInGame())) {
            overlay::OnKeyDown(vk);
        }
    }
    if (message == WM_CHAR && g_swallowChar != 0) {
        const wchar_t expected = g_swallowChar;
        g_swallowChar = 0;
        if (towlower(static_cast<wint_t>(wParam)) == static_cast<wint_t>(expected)) {
            return 0;
        }
    }

    // Mensaje propio: sincronizar las teclas sostenidas (no es para el juego).
    if (message == kHoldSyncMessage) {
        SyncHolds(window);
        return 0;
    }
    if (message == kTapKeyMessage) {
        SendKeyToGame(window, static_cast<int>(wParam), true);
        SendKeyToGame(window, static_cast<int>(wParam), false);
        return 0;
    }
    if (message == kFreecamReleaseMessage) {
        // Free cam: "soltarle" al juego las teclas de movimiento (si tenías W
        // apretada al activarlo, el jugador seguiría caminando para siempre).
        constexpr int kRelease[] = {'W', 'A', 'S', 'D', VK_SPACE, VK_SHIFT, VK_CONTROL};
        for (int vk : kRelease) {
            SendKeyToGame(window, vk, false);
        }
        return 0;
    }
    if (message == kToggleMenuMessage) {
        overlay::ToggleMenu();
        return 0;
    }
    // Al volver a la ventana, el juego puede haber "soltado" todo: volvemos
    // a apretar las teclas sostenidas.
    if (message == WM_SETFOCUS) {
        for (Hold& h : g_holds) {
            h.applied.store(false);
        }
        const LRESULT result = CallWindowProcW(g_originalWndProc.load(), window, message, wParam, lParam);
        SyncHolds(window);
        return result;
    }

    if (!overlay::IsMenuOpen()) {
        if (message == WM_MOUSEWHEEL && g_wheelCapture.load(std::memory_order_relaxed)) {
            g_wheelDelta.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;  // el juego no cambia de ranura mientras haces zoom
        }
        if (g_freecamKeys.load(std::memory_order_relaxed) &&
            (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
            IsFreecamKey(static_cast<int>(wParam))) {
            return 0;  // free cam: el jugador no camina (ya se le "soltaron" al activarlo)
        }
        if (HandleHoldKey(message, wParam, lParam)) {
            return 0;
        }
        // DROPPING AN ITEM while Luma holds Ctrl for you (toggle sprint): in
        // Bedrock Ctrl+Q drops the WHOLE stack. So for the moment of the Q,
        // Ctrl is let go for the game (the messages and GetKeyState agree),
        // and pressed again after. Only if YOU aren't really holding Ctrl
        // (then Ctrl+Q is on purpose).
        if ((message == WM_KEYDOWN || message == WM_KEYUP) && wParam == 'Q') {
            for (Hold& h : g_holds) {
                const bool ctrl = SameKey(VK_CONTROL, h.vk.load());
                const bool reallyHeld = (g_originalGetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                if (!ctrl || reallyHeld) continue;
                if (message == WM_KEYDOWN && h.applied.load() && (lParam & (1 << 30)) == 0) {
                    SendKeyToGame(window, h.appliedVk, false);
                    h.applied.store(false);
                    g_dropPaused.store(true);
                } else if (message == WM_KEYUP && g_dropPaused.exchange(false)) {
                    const LRESULT result = CallWindowProcW(g_originalWndProc.load(), window, message, wParam, lParam);
                    SyncHolds(window);  // (Ctrl pressed again for the game: you keep sprinting)
                    return result;
                }
            }
        }
        // Respaldo del contador de clics si no está la cola del mouse del juego.
        if (clicks::Wanted() && !sdk::mousequeue::Available()) {
            if (message == WM_LBUTTONDOWN) clicks::Record(clicks::Button::Left);
            if (message == WM_RBUTTONDOWN) clicks::Record(clicks::Button::Right);
        }
    }

    if (overlay::IsMenuOpen()) {
        overlay::FeedInput(window, message, wParam, lParam);
        // Los mensajes de SOLTAR (tecla o botón) siempre le llegan al juego:
        // si soltaste W con el menú abierto, el juego tiene que enterarse, o
        // seguiría caminando para siempre (lleva la cuenta de las teclas
        // apretadas con estos mensajes).
        if (IsInputMessage(message) && !IsReleaseMessage(message)) {
            // WM_INPUT (input "crudo") debe pasar por DefWindowProc para que
            // Windows libere sus datos internos. Los de puntero también:
            // DefWindowProc los convierte en mensajes de mouse clásicos, que
            // vuelven a pasar por aquí (y los recibe ImGui).
            if (message == WM_INPUT || IsPointerMessage(message)) {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return 0;
        }
    }

    // Todo lo demás (o todo, con el menú cerrado) sigue hacia el juego.
    return CallWindowProcW(g_originalWndProc.load(), window, message, wParam, lParam);
}

// --- Hooks ----------------------------------------------------------------------------

// PeekMessageW: con el menú abierto, cuando el juego saca un mensaje de input
// de su ventana, Luma lo procesa ahí mismo (como si hubiera llegado a la
// WndProc: ImGui lo recibe, la tecla del menú funciona) y le devuelve al
// juego un mensaje vacío (WM_NULL), que no hace nada.
BOOL WINAPI PeekMessageWDetour(LPMSG msg, HWND window, UINT filterMin, UINT filterMax,
                               UINT removeFlags) {
    hooks::ActiveCallGuard guard;
    const BOOL got = g_originalPeekMessageW(msg, window, filterMin, filterMax, removeFlags);
    if (!got || msg == nullptr || g_window == nullptr || msg->hwnd != g_window ||
        !IsInputMessage(msg->message) || IsReleaseMessage(msg->message) || !overlay::IsMenuOpen() ||
        !g_hooksActive.load(std::memory_order_relaxed)) {
        return got;
    }
    if (removeFlags & PM_REMOVE) {
        // TranslateMessage genera el WM_CHAR (el carácter) de una tecla; lo
        // necesita ImGui para escribir en la búsqueda. Llega en una vuelta
        // siguiente y pasa por aquí mismo.
        TranslateMessage(msg);
        LumaWndProc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    }
    // Si el juego solo "miraba" el mensaje (PM_NOREMOVE), también lo ve vacío;
    // lo procesamos cuando lo saque de verdad.
    msg->message = WM_NULL;
    return got;
}

// GetAsyncKeyState / GetKeyState: "¿está apretada esta tecla o botón ahora?".
// Con el menú abierto le respondemos al juego "no" para todo. Luma (ImGui y la
// tecla de descarga) sigue viendo la verdad.
bool BlockKeyStateForCaller(void* returnAddress) {
    return overlay::IsMenuOpen() && g_hooksActive.load(std::memory_order_relaxed) &&
           !IsLumaAddress(returnAddress);
}

// Si el juego pregunta por una tecla sostenida por Luma, le decimos que
// está apretada (bit 0x8000), para que coincida con los mensajes.
SHORT ApplyHolds(int vk, SHORT state, void* returnAddress) {
    if (IsLumaAddress(returnAddress)) {
        return state;
    }
    for (const Hold& h : g_holds) {
        if (h.applied.load(std::memory_order_relaxed) && SameKey(vk, h.vk.load(std::memory_order_relaxed))) {
            return static_cast<SHORT>(state | static_cast<SHORT>(0x8000));
        }
    }
    return state;
}

// Free cam: si el juego pregunta por una tecla de movimiento, "no está apretada".
bool BlockFreecamKey(int vk, void* returnAddress) {
    return g_freecamKeys.load(std::memory_order_relaxed) && IsFreecamKey(vk) && !IsLumaAddress(returnAddress);
}

SHORT WINAPI GetAsyncKeyStateDetour(int vk) {
    hooks::ActiveCallGuard guard;
    if (BlockKeyStateForCaller(_ReturnAddress()) || BlockFreecamKey(vk, _ReturnAddress())) {
        return 0;
    }
    return ApplyHolds(vk, g_originalGetAsyncKeyState(vk), _ReturnAddress());
}

SHORT WINAPI GetKeyStateDetour(int vk) {
    hooks::ActiveCallGuard guard;
    if (BlockKeyStateForCaller(_ReturnAddress()) || BlockFreecamKey(vk, _ReturnAddress())) {
        return 0;
    }
    return ApplyHolds(vk, g_originalGetKeyState(vk), _ReturnAddress());
}

BOOL WINAPI ClipCursorDetour(const RECT* rect) {
    hooks::ActiveCallGuard guard;
    {
        // Recordamos SIEMPRE lo que pide el juego, por si hay que restaurarlo.
        std::lock_guard lock(g_clipMutex);
        g_gameClipKnown = true;
        g_gameClipActive = rect != nullptr;
        if (rect != nullptr) {
            g_gameClipRect = *rect;
        }
    }
    // Con el menú abierto, el juego no puede ENCERRAR el cursor (quedaría
    // clavado y no podrías usar el menú). Liberarlo (rect == nullptr) sí se
    // permite siempre: es justo lo que hace el juego al soltar el mouse.
    if (rect != nullptr && overlay::IsMenuOpen() && g_hooksActive.load(std::memory_order_relaxed)) {
        return TRUE;
    }
    return g_originalClipCursor(rect);
}

// Al abrir el menú: avisarle al juego que se soltaron todas las teclas y
// botones que estaban apretados. Si no, el juego seguiría "viendo" W
// apretada (y caminando) mientras el menú está abierto.
// Se llama desde el hilo de la ventana (donde el juego procesa sus mensajes).
void ReleaseHeldInput(HWND window) {
    WNDPROC original = g_originalWndProc.load();
    if (window == nullptr || original == nullptr || g_originalGetAsyncKeyState == nullptr) {
        return;
    }
    // El bit más alto (0x8000) de GetAsyncKeyState = "apretada ahora mismo".
    auto held = [](int vk) { return (g_originalGetAsyncKeyState(vk) & 0x8000) != 0; };
    int released = 0;

    // Teclas: WM_KEYUP con el formato de lParam que manda Windows
    // (bit 31 = se soltó, bit 30 = estaba apretada, repetición 1, código de escaneo).
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (IsMouseButtonKey(vk) || !held(vk)) {
            continue;
        }
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
        const LPARAM lParam = static_cast<LPARAM>(0xC0000001u | (scan << 16));
        CallWindowProcW(original, window, WM_KEYUP, static_cast<WPARAM>(vk), lParam);
        ++released;
    }

    // Botones del mouse: WM_xBUTTONUP en la posición actual del cursor.
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        ScreenToClient(window, &cursor);
    }
    const LPARAM position = MAKELPARAM(cursor.x, cursor.y);
    const struct {
        int vk;
        UINT message;
        WPARAM wParam;
    } buttons[] = {{VK_LBUTTON, WM_LBUTTONUP, 0},
                   {VK_RBUTTON, WM_RBUTTONUP, 0},
                   {VK_MBUTTON, WM_MBUTTONUP, 0},
                   {VK_XBUTTON1, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1)},
                   {VK_XBUTTON2, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2)}};
    for (const auto& b : buttons) {
        if (held(b.vk)) {
            CallWindowProcW(original, window, b.message, b.wParam, position);
            ++released;
        }
    }
    if (released > 0) {
        log::Info("Menú abierto: se soltaron {} teclas/botones que estaban apretados.", released);
    }
}

}  // namespace

bool CreateHooks() {
    // Rango de Luma.dll en memoria: desde __ImageBase hasta su tamaño (que
    // está escrito en la cabecera del propio archivo, "SizeOfImage").
    g_lumaStart = reinterpret_cast<std::uintptr_t>(&__ImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const std::uint8_t*>(&__ImageBase) + __ImageBase.e_lfanew);
    g_lumaEnd = g_lumaStart + nt->OptionalHeader.SizeOfImage;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    struct HookSpec {
        const char* exportName;
        void* detour;
        void** original;
        const char* logName;
    };
    const HookSpec specs[] = {
        {"PeekMessageW", reinterpret_cast<void*>(&PeekMessageWDetour),
         reinterpret_cast<void**>(&g_originalPeekMessageW), "user32!PeekMessageW"},
        {"GetAsyncKeyState", reinterpret_cast<void*>(&GetAsyncKeyStateDetour),
         reinterpret_cast<void**>(&g_originalGetAsyncKeyState), "user32!GetAsyncKeyState"},
        {"GetKeyState", reinterpret_cast<void*>(&GetKeyStateDetour),
         reinterpret_cast<void**>(&g_originalGetKeyState), "user32!GetKeyState"},
        {"ClipCursor", reinterpret_cast<void*>(&ClipCursorDetour),
         reinterpret_cast<void**>(&g_originalClipCursor), "user32!ClipCursor"},
    };

    bool ok = true;
    for (const HookSpec& spec : specs) {
        void* target = reinterpret_cast<void*>(GetProcAddress(user32, spec.exportName));
        if (target == nullptr) {
            log::Error("No se encontró {} en user32.dll.", spec.exportName);
            ok = false;
            continue;
        }
        ok &= hooks::Create(target, spec.detour, spec.original, spec.logName);
    }
    g_hooksActive.store(ok);
    return ok;
}

bool Install(HWND gameWindow) {
    if (g_window != nullptr) {
        return true;  // Ya instalada.
    }
    // SetWindowLongPtrW con GWLP_WNDPROC cambia la WndProc de la ventana y
    // devuelve la anterior. Guardamos la original ANTES de instalar la nuestra,
    // porque la nuestra puede empezar a recibir mensajes enseguida.
    g_originalWndProc.store(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC)));
    const LONG_PTR previous =
        SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&LumaWndProc));
    if (previous == 0) {
        log::Error("No se pudo instalar el procesador de input (error {}).", GetLastError());
        return false;
    }
    g_originalWndProc.store(reinterpret_cast<WNDPROC>(previous));
    g_window = gameWindow;
    log::Info("Procesador de input instalado en la ventana 0x{:X}.",
              reinterpret_cast<std::uintptr_t>(gameWindow));
    return true;
}

void OnMenuToggled(bool open) {
    if (open) {
        // Que el juego suelte lo que tenía apretado (si no, sigue caminando).
        ReleaseHeldInput(g_window);
        // Pedirle al juego que suelte el mouse, como hace la pausa
        // (sdk/mouse_grab.h): muestra y libera el cursor, y deja de tratar el
        // mouse como "de juego".
        sdk::mouse::OnMenuOpened();
        // Red de seguridad: liberar el cursor nosotros también (si el juego ya
        // lo liberó no pasa nada). Si el camino del juego no estuviera
        // disponible, recordamos cómo estaba para devolverlo al cerrar.
        if (!sdk::mouse::ReleasedByLuma()) {
            std::lock_guard lock(g_clipMutex);
            RECT current{};
            if (!g_gameClipKnown && GetClipCursor(&current)) {
                g_gameClipKnown = true;
                g_gameClipActive = true;
                g_gameClipRect = current;
            }
        }
        RealClipCursor(nullptr);
    } else {
        // Si el juego soltó el mouse por nosotros, al cerrar lo vuelve a
        // agarrar él mismo (y encierra el cursor como siempre). Si no,
        // devolvemos el encierro que tenía.
        const bool releasedByGame = sdk::mouse::ReleasedByLuma();
        sdk::mouse::OnMenuClosed();
        if (!releasedByGame) {
            RestoreGameClip();
        }
    }
}

void Uninstall() {
    // Soltar las teclas sostenidas. SendMessageTimeout espera a que el hilo
    // de la ventana lo procese (máximo 0,5 s, por si estuviera trabado).
    for (Hold& h : g_holds) {
        h.enabled.store(false);
    }
    if (g_window != nullptr) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(g_window, kHoldSyncMessage, 0, 0, SMTO_ABORTIFHUNG, 500, &ignored);
    }
    g_hooksActive.store(false);
    if (overlay::IsMenuOpen() && !sdk::mouse::ReleasedByLuma()) {
        RestoreGameClip();  // Devolver el cursor como lo quería el juego.
    }
    if (g_window == nullptr) {
        return;
    }
    // Si otro programa (por ejemplo, otro overlay) instaló su propia WndProc
    // DESPUÉS que nosotros, al restaurar la original le cortamos la cadena.
    // No hay forma perfecta de evitarlo, así que al menos lo dejamos en el log.
    const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
    if (current != &LumaWndProc) {
        log::Warn("Otro programa cambió la WndProc del juego después de Luma.");
    }
    SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc.load()));
    g_window = nullptr;
    log::Info("Procesador de input restaurado.");
}

HWND GameWindow() {
    return g_window;
}

void RequestMenuToggle() {
    if (g_window != nullptr) {
        PostMessageW(g_window, kToggleMenuMessage, 0, 0);
    }
}

void ConfigureHold(HoldSlot slot, bool enabled, int vk) {
    Hold& h = g_holds[static_cast<int>(slot)];
    h.vk.store(vk);
    if (enabled && !h.enabled.load()) {
        h.active.store(true);  // al activar el módulo, empieza sosteniendo
    }
    h.enabled.store(enabled);
    PostHoldSync();
}

bool IsHoldActive(HoldSlot slot) {
    const Hold& h = g_holds[static_cast<int>(slot)];
    return h.enabled.load(std::memory_order_relaxed) && h.active.load(std::memory_order_relaxed);
}

void SetWheelCapture(bool capture) {
    g_wheelCapture.store(capture);
    sdk::mousequeue::SetBlockWheel(capture);
}

void TapKey(int vk) {
    if (g_window != nullptr) {
        PostMessageW(g_window, kTapKeyMessage, static_cast<WPARAM>(vk), 0);
    }
}

void SetFreecamKeys(bool block) {
    const bool was = g_freecamKeys.exchange(block);
    if (block && !was && g_window != nullptr) {
        PostMessageW(g_window, kFreecamReleaseMessage, 0, 0);
    }
}

int TakeWheelSteps() {
    return g_wheelDelta.exchange(0) / WHEEL_DELTA;
}

void PollHolds() {
    if (g_window == nullptr || (!g_holds[0].enabled.load() && !g_holds[1].enabled.load())) {
        return;
    }
    // Suspender si la ventana no tiene el foco, si el menú de Luma está
    // abierto o si estás en un menú del juego (el juego soltó el mouse). Si
    // eso no se sabe (firmas no encontradas), miramos si el cursor está
    // visible: jugando, el juego lo esconde.
    bool suspended = GetForegroundWindow() != g_window || overlay::IsMenuOpen();
    if (!suspended) {
        const int mouse = sdk::mouse::GameMouseState();
        if (mouse == 0) {
            suspended = true;
        } else if (mouse < 0) {
            CURSORINFO info{};
            info.cbSize = sizeof(info);
            suspended = GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING) != 0;
        }
    }
    if (g_holdsSuspended.exchange(suspended) != suspended) {
        PostHoldSync();
    }
}

}  // namespace luma::render::input
