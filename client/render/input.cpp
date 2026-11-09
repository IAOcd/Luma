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

#include <atomic>
#include <cstdint>
#include <mutex>

#include "core/hooks.h"
#include "core/logger.h"
#include "render/overlay.h"

namespace luma::render::input {

namespace {

// Tecla para abrir/cerrar el menú: L. (Los códigos de las letras son su
// mayúscula en ASCII: 'L' = 0x4C.) Será configurable en la Fase 3.
//
// LIMITACIÓN CONOCIDA: Luma todavía no sabe si el chat del juego está
// abierto, así que escribir una "l" en el chat abre el menú. Se corrige en la
// Fase 7, cuando Luma pueda leer ese estado del juego.
constexpr WPARAM kMenuKey = 'L';

HWND g_window = nullptr;
// La WndProc original del juego. Atómica porque la leen otros hilos.
std::atomic<WNDPROC> g_originalWndProc{nullptr};

// Tras abrir/cerrar el menú con L, Windows genera además un WM_CHAR 'l'
// (el carácter). Lo descartamos para que no se escriba en el chat del juego.
bool g_swallowNextChar = false;

// --- Hooks de Windows -----------------------------------------------------------
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
GetRawInputDataFn g_originalGetRawInputData = nullptr;
ClipCursorFn g_originalClipCursor = nullptr;

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

    // Solo nos interesa cuando el juego pidió los datos completos (RID_INPUT)
    // y la llamada salió bien. ((UINT)-1 es como Windows indica error.)
    if (data != nullptr && command == RID_INPUT && result != static_cast<UINT>(-1) &&
        result >= sizeof(RAWINPUTHEADER) && overlay::IsMenuOpen() &&
        g_hooksActive.load(std::memory_order_relaxed)) {
        auto* raw = static_cast<RAWINPUT*>(data);
        if (raw->header.dwType == RIM_TYPEMOUSE) {
            // Mouse "quieto": sin movimiento, sin botones, sin rueda.
            raw->data.mouse.lLastX = 0;
            raw->data.mouse.lLastY = 0;
            raw->data.mouse.usButtonFlags = 0;
            raw->data.mouse.usButtonData = 0;
            raw->data.mouse.ulRawButtons = 0;
            g_blockedMouseEvents.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
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

    // Tecla del menú. Si ImGui está esperando texto (por ejemplo, una caja de
    // búsqueda en el futuro), la L se escribe en vez de cerrar el menú.
    if (message == WM_KEYDOWN && wParam == kMenuKey && !overlay::WantsTextInput()) {
        // El bit 30 de lParam indica "la tecla ya estaba apretada" (repetición
        // automática al mantenerla): la ignoramos para que no parpadee.
        const bool isRepeat = (lParam & (1 << 30)) != 0;
        if (!isRepeat) {
            overlay::ToggleMenu();
        }
        g_swallowNextChar = true;
        return 0;  // El juego no ve esta L.
    }
    if (message == WM_CHAR && g_swallowNextChar) {
        g_swallowNextChar = false;
        if (wParam == 'l' || wParam == 'L') {
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

}  // namespace

bool CreateHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* getRawInputData = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"));
    void* clipCursor = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
    if (getRawInputData == nullptr || clipCursor == nullptr) {
        log::Error("No se encontraron GetRawInputData / ClipCursor en user32.dll.");
        return false;
    }
    bool ok = hooks::Create(getRawInputData, reinterpret_cast<void*>(&GetRawInputDataDetour),
                            reinterpret_cast<void**>(&g_originalGetRawInputData),
                            "user32!GetRawInputData");
    ok &= hooks::Create(clipCursor, reinterpret_cast<void*>(&ClipCursorDetour),
                        reinterpret_cast<void**>(&g_originalClipCursor), "user32!ClipCursor");
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
        RealClipCursor(nullptr);  // Liberar el cursor.
        g_blockedMouseEvents.store(0);
    } else {
        RestoreGameClip();
        log::Info("Menú cerrado. Eventos de mouse ocultados al juego: {}.",
                  g_blockedMouseEvents.load());
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
