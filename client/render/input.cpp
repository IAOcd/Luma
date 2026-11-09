// =============================================================================
//  input.cpp - Nuestra WndProc y los hooks de raw input / ClipCursor
// =============================================================================
//  Qué descubrimos al probar (Fase 2a): con el menú abierto el cursor de
//  ImGui no se movía y la cámara del juego sí. Revisando qué funciones de
//  Windows importa Minecraft.Windows.exe (con la herramienta "dumpbin"):
//
//    - GetRawInputData: el juego lee el movimiento del mouse como "raw input"
//      (datos crudos del mouse) directamente desde su cola de mensajes, ANTES
//      de que lleguen a la WndProc. Bloquearlo en la WndProc no alcanza.
//    - ClipCursor: dentro de un mundo, el juego encierra el cursor de Windows
//      para que no salga de la ventana. Por eso ImGui no veía que se moviera.
//
//  Solución (todo funciones de Windows, estables entre versiones del juego):
//    - Con el menú abierto, GetRawInputData le devuelve al juego un mouse
//      "quieto": movimiento 0 y sin clics. La cámara no se mueve.
//    - Con el menú abierto, ignoramos los ClipCursor del juego y liberamos el
//      cursor. Al cerrar el menú, restauramos lo último que el juego pidió.
// =============================================================================
#include "render/input.h"

#include <intrin.h>  // _ReturnAddress: desde dónde nos llamaron

#include <atomic>
#include <cstdint>
#include <cwctype>
#include <mutex>

#include "core/config.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "render/input_diag.h"
#include "render/overlay.h"

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

// Tras una tecla que usó Luma (por ejemplo, abrir el menú con L), Windows
// genera además un WM_CHAR con el carácter ('l'). Lo descartamos para que
// no se escriba en el chat del juego. 0 = no descartar nada.
wchar_t g_swallowChar = 0;

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

// --- Hooks de Windows -----------------------------------------------------------
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
GetRawInputDataFn g_originalGetRawInputData = nullptr;
GetRawInputBufferFn g_originalGetRawInputBuffer = nullptr;
GetCursorPosFn g_originalGetCursorPos = nullptr;
ClipCursorFn g_originalClipCursor = nullptr;

using PeekMessageWFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using KeyStateFn = SHORT(WINAPI*)(int);
PeekMessageWFn g_originalPeekMessageW = nullptr;
KeyStateFn g_originalGetAsyncKeyState = nullptr;
KeyStateFn g_originalGetKeyState = nullptr;

// Rango de memoria donde está cargado Luma.dll. Sirve para saber si quien
// llama a GetCursorPos es Luma (ImGui necesita la posición real) o el juego.
// "__ImageBase" es un símbolo que el enlazador de Microsoft define al inicio
// de nuestro propio módulo.
extern "C" IMAGE_DOS_HEADER __ImageBase;
std::uintptr_t g_lumaStart = 0;
std::uintptr_t g_lumaEnd = 0;

bool IsLumaAddress(void* address) {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= g_lumaStart && value < g_lumaEnd;
}

// Posición del cursor al abrir el menú: es la que ve el juego mientras esté abierto.
std::atomic<LONG> g_frozenCursorX{0};
std::atomic<LONG> g_frozenCursorY{0};

void ClearMouseData(RAWINPUT* raw) {
    raw->data.mouse.lLastX = 0;
    raw->data.mouse.lLastY = 0;
    raw->data.mouse.usButtonFlags = 0;
    raw->data.mouse.usButtonData = 0;
    raw->data.mouse.ulRawButtons = 0;
}

// Último encierro de cursor que pidió el juego (para restaurarlo).
std::mutex g_clipMutex;
bool g_gameClipKnown = false;
bool g_gameClipActive = false;  // false = el juego pidió "sin encierro"
RECT g_gameClipRect{};

// Diagnóstico: cuántos eventos de mouse le escondimos al juego.
std::atomic<std::uint32_t> g_blockedMouseEvents{0};

std::atomic<bool> g_hooksActive{false};

UINT WINAPI GetRawInputDataDetour(HRAWINPUT rawInput, UINT command, LPVOID data, PUINT size,
                                  UINT headerSize) {
    hooks::ActiveCallGuard guard;
    const UINT result = g_originalGetRawInputData(rawInput, command, data, size, headerSize);
    if (data != nullptr) {
        diag::Count(diag::g_counters.getRawInputData);
    }

    // Solo nos interesa cuando el juego pidió los datos completos (RID_INPUT)
    // y la llamada salió bien. ((UINT)-1 es como Windows indica error.)
    if (data != nullptr && command == RID_INPUT && result != static_cast<UINT>(-1) &&
        result >= sizeof(RAWINPUTHEADER) && overlay::IsMenuOpen() &&
        g_hooksActive.load(std::memory_order_relaxed)) {
        auto* raw = static_cast<RAWINPUT*>(data);
        if (raw->header.dwType == RIM_TYPEMOUSE) {
            ClearMouseData(raw);  // Mouse "quieto": sin movimiento, botones ni rueda.
            g_blockedMouseEvents.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
}

// GetRawInputBuffer lee VARIOS eventos de raw input de una vez (lo usan
// algunos motores para rendimiento). No aparece en las importaciones del
// juego, pero podría cargarse dinámicamente, así que lo cubrimos también.
UINT WINAPI GetRawInputBufferDetour(PRAWINPUT data, PUINT size, UINT headerSize) {
    hooks::ActiveCallGuard guard;
    const UINT count = g_originalGetRawInputBuffer(data, size, headerSize);
    if (data == nullptr || count == 0 || count == static_cast<UINT>(-1)) {
        return count;
    }
    diag::Count(diag::g_counters.getRawInputBuffer);
    if (overlay::IsMenuOpen() && g_hooksActive.load(std::memory_order_relaxed)) {
        // Los eventos vienen uno detrás de otro, cada uno alineado a 8 bytes.
        // (Es lo que hace la macro NEXTRAWINPUTBLOCK de Windows, que necesita
        // un tipo QWORD que no siempre está definido.)
        auto next = [](RAWINPUT* block) {
            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(block) +
                                           block->header.dwSize + 7;
            return reinterpret_cast<RAWINPUT*>(address & ~static_cast<std::uintptr_t>(7));
        };
        RAWINPUT* raw = data;
        for (UINT i = 0; i < count; ++i) {
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                ClearMouseData(raw);
                g_blockedMouseEvents.fetch_add(1, std::memory_order_relaxed);
            }
            raw = next(raw);
        }
    }
    return count;
}

// Si el juego calcula el movimiento de la cámara con la posición del cursor,
// con el menú abierto le mostramos siempre la misma posición. A Luma (ImGui)
// le damos la real.
BOOL WINAPI GetCursorPosDetour(LPPOINT point) {
    hooks::ActiveCallGuard guard;
    const BOOL ok = g_originalGetCursorPos(point);
    if (!IsLumaAddress(_ReturnAddress())) {
        diag::Count(diag::g_counters.getCursorPosGame);
        if (ok && point != nullptr && overlay::IsMenuOpen() &&
            g_hooksActive.load(std::memory_order_relaxed)) {
            point->x = g_frozenCursorX.load(std::memory_order_relaxed);
            point->y = g_frozenCursorY.load(std::memory_order_relaxed);
        }
    }
    return ok;
}

BOOL WINAPI ClipCursorDetour(const RECT* rect) {
    hooks::ActiveCallGuard guard;
    {
        // Recordamos SIEMPRE lo que pide el juego, para restaurarlo después.
        std::lock_guard lock(g_clipMutex);
        g_gameClipKnown = true;
        g_gameClipActive = rect != nullptr;
        if (rect != nullptr) {
            g_gameClipRect = *rect;
        }
    }
    if (overlay::IsMenuOpen() && g_hooksActive.load(std::memory_order_relaxed)) {
        return TRUE;  // Con el menú abierto, el cursor queda libre.
    }
    return g_originalClipCursor(rect);
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

// --- WndProc --------------------------------------------------------------------

// ¿Es un mensaje de teclado o de mouse? (Los que bloqueamos con el menú abierto.)
bool IsInputMessage(UINT message) {
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
           (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT;
}

LRESULT CALLBACK LumaWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Contamos esta llamada como "activa" para que la descarga espere a que
    // termine (ver hooks.h).
    hooks::ActiveCallGuard guard;

    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
        diag::Count(diag::g_counters.wndMouseMessages);
    } else if (message == WM_INPUT) {
        diag::Count(diag::g_counters.wndRawInputMessages);
    }

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
        // 2. La tecla del menú. Si ImGui espera texto (la caja de búsqueda),
        //    la letra se escribe en vez de cerrar el menú.
        if (vk == config::g_menuKey.load(std::memory_order_relaxed) && !overlay::WantsTextInput()) {
            if (!isRepeat) {
                overlay::ToggleMenu();
            }
            g_swallowChar = keyDown ? CharForKey(vk) : 0;
            return 0;  // El juego no ve esta tecla.
        }
        // 3. Teclas de los módulos (el juego también las recibe).
        if (!isRepeat && !overlay::WantsTextInput()) {
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

    if (overlay::IsMenuOpen()) {
        overlay::FeedInput(window, message, wParam, lParam);
        if (IsInputMessage(message)) {
            // WM_INPUT (input "crudo" del mouse) debe pasar por DefWindowProc
            // para que Windows libere sus datos internos. Los demás simplemente
            // no se le entregan al juego.
            if (message == WM_INPUT) {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return 0;
        }
    }

    // Todo lo demás (o todo, con el menú cerrado) sigue hacia el juego.
    return CallWindowProcW(g_originalWndProc.load(), window, message, wParam, lParam);
}

// --- Hooks de la cola de mensajes y del estado de teclas -------------------------

// PeekMessageW: el juego saca los mensajes de su cola con esta función. Muchos
// motores miran los mensajes de mouse y teclado ANTES de entregarlos a la
// ventana (DispatchMessage), así que bloquearlos en la WndProc no alcanza.
//
// Con el menú abierto, cuando el juego saca un mensaje de input de su
// ventana, Luma lo procesa ahí mismo (como si hubiera llegado a la WndProc:
// ImGui lo recibe, la tecla del menú funciona) y le devuelve al juego un
// mensaje vacío (WM_NULL), que no hace nada.
BOOL WINAPI PeekMessageWDetour(LPMSG msg, HWND window, UINT filterMin, UINT filterMax,
                               UINT removeFlags) {
    hooks::ActiveCallGuard guard;
    const BOOL got = g_originalPeekMessageW(msg, window, filterMin, filterMax, removeFlags);
    if (!got || msg == nullptr || g_window == nullptr || msg->hwnd != g_window ||
        !IsInputMessage(msg->message) || !overlay::IsMenuOpen() ||
        !g_hooksActive.load(std::memory_order_relaxed)) {
        return got;
    }
    if (removeFlags & PM_REMOVE) {
        diag::Count(diag::g_counters.peekMessageBlocked);
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
// Con el menú abierto le respondemos al juego "no" para todo (así no ataca
// ni camina). Luma (ImGui y la tecla de descarga) sigue viendo la verdad.
bool BlockKeyStateForCaller(void* returnAddress) {
    return overlay::IsMenuOpen() && g_hooksActive.load(std::memory_order_relaxed) &&
           !IsLumaAddress(returnAddress);
}

SHORT WINAPI GetAsyncKeyStateDetour(int vk) {
    hooks::ActiveCallGuard guard;
    void* caller = _ReturnAddress();
    if (!IsLumaAddress(caller)) {
        diag::Count(diag::g_counters.getAsyncKeyStateGame);
    }
    if (BlockKeyStateForCaller(caller)) {
        return 0;
    }
    return g_originalGetAsyncKeyState(vk);
}

SHORT WINAPI GetKeyStateDetour(int vk) {
    hooks::ActiveCallGuard guard;
    void* caller = _ReturnAddress();
    if (!IsLumaAddress(caller)) {
        diag::Count(diag::g_counters.getKeyStateGame);
    }
    if (BlockKeyStateForCaller(caller)) {
        return 0;
    }
    return g_originalGetKeyState(vk);
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
        {"GetRawInputData", reinterpret_cast<void*>(&GetRawInputDataDetour),
         reinterpret_cast<void**>(&g_originalGetRawInputData), "user32!GetRawInputData"},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&GetRawInputBufferDetour),
         reinterpret_cast<void**>(&g_originalGetRawInputBuffer), "user32!GetRawInputBuffer"},
        {"GetCursorPos", reinterpret_cast<void*>(&GetCursorPosDetour),
         reinterpret_cast<void**>(&g_originalGetCursorPos), "user32!GetCursorPos"},
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
        {
            // Si el juego encerró el cursor antes de que Luma se cargara, no
            // pasó por nuestro hook: lo leemos de Windows para poder restaurarlo.
            std::lock_guard lock(g_clipMutex);
            if (!g_gameClipKnown) {
                RECT current{};
                if (GetClipCursor(&current)) {
                    g_gameClipKnown = true;
                    g_gameClipActive = true;
                    g_gameClipRect = current;
                }
            }
        }
        // Posición que verá el juego mientras el menú esté abierto.
        POINT cursor{};
        if (g_originalGetCursorPos != nullptr && g_originalGetCursorPos(&cursor)) {
            g_frozenCursorX.store(cursor.x);
            g_frozenCursorY.store(cursor.y);
        }
        RealClipCursor(nullptr);  // Liberar el cursor.
        g_blockedMouseEvents.store(0);
        diag::LogAndReset("antes de abrir el menú");
    } else {
        // Devolver el cursor a donde estaba al abrir el menú: así, para el
        // juego, el cursor nunca se movió y la cámara no salta al cerrar.
        SetCursorPos(g_frozenCursorX.load(), g_frozenCursorY.load());
        RestoreGameClip();
        log::Info("Menú cerrado. Eventos de raw input de mouse ocultados al juego: {}.",
                  g_blockedMouseEvents.load());
        diag::LogAndReset("con el menú abierto");
    }
}

void Uninstall() {
    g_hooksActive.store(false);
    if (overlay::IsMenuOpen()) {
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

}  // namespace luma::render::input

namespace luma::render::diag {

void LogAndReset(const char* context) {
    auto take = [](std::atomic<std::uint32_t>& counter) { return counter.exchange(0); };
    log::Info("Diagnóstico de input ({}): mensajes de mouse a la ventana={}, WM_INPUT={}, "
              "GetRawInputData={}, GetRawInputBuffer={}, GetCursorPos del juego={}, "
              "GameInput lecturas pedidas={}, GameInput GetMouseState={}, GameInput GetKeyState={}, "
              "PeekMessage interceptados={}, GetAsyncKeyState del juego={}, GetKeyState del juego={}",
              context, take(g_counters.wndMouseMessages), take(g_counters.wndRawInputMessages),
              take(g_counters.getRawInputData), take(g_counters.getRawInputBuffer),
              take(g_counters.getCursorPosGame), take(g_counters.gameInputGetReading),
              take(g_counters.gameInputMouseState),
              take(g_counters.gameInputKeyState), take(g_counters.peekMessageBlocked),
              take(g_counters.getAsyncKeyStateGame), take(g_counters.getKeyStateGame));
}

}  // namespace luma::render::diag
