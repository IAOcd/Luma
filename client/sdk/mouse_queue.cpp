// =============================================================================
//  mouse_queue.cpp - Implementación del bloqueo de la cola del mouse
// =============================================================================
#include "sdk/mouse_queue.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/clicks.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "render/notifications.h"
#include "render/overlay.h"
#include "sdk/mouse_grab.h"
#include "sdk/scanner.h"

namespace luma::sdk::mousequeue {

namespace {

// --- Firma (Minecraft Bedrock GDK 1.26.63; función en juego+0x427EF0) -------------
// Un trozo del inicio de la función que procesa la cola (a 0x20 bytes de su
// comienzo):
//
//   mov r14, rcx
//   mov dword [MouseDevice], -1        ; C7 05 <X> FF FF FF FF
//   mov rcx, [lista.final]             ; 48 8B 0D <X>
//   mov rax, [lista.inicio]            ; 48 8B 05 <X>
//   sub rcx, rax
//   shr rcx, 2                         ; \  (final - inicio) / 20 =
//   imul ecx, ecx, 0CCCCCCCDh          ; /  cantidad de acciones
//
// Las tres variables globales salen de las propias instrucciones (así una
// actualización que las mueva no rompe nada).
constexpr const char* kQueuePattern =
    "49 89 CE C7 05 ?? ?? ?? ?? FF FF FF FF 48 8B 0D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
    "48 29 C1 48 C1 E9 02 69 C9 CD CC CC CC";
constexpr std::uintptr_t kPatternOffsetInFunction = 0x20;  // la firma empieza 0x20 bytes adentro
// Inicio esperado de la función: push r15; push r14; push r13; push r12.
constexpr std::uint8_t kExpectedPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};

constexpr std::size_t kActionSize = 20;   // bytes por acción (ver mouse_queue.h)
constexpr std::uint8_t kActionMove = 0;
constexpr std::uint8_t kActionLeft = 1;
constexpr std::uint8_t kActionRight = 2;

// La función recibe tres parámetros (rcx, rdx, r8) que no necesitamos
// entender: solo se los pasamos a la original.
using ProcessQueueFn = std::uintptr_t(__fastcall*)(void*, void*, void*);
ProcessQueueFn g_original = nullptr;

std::uintptr_t g_device = 0;     // dirección del MouseDevice global
std::uintptr_t g_listBegin = 0;  // dirección de lista.inicio (un puntero)
std::uintptr_t g_listEnd = 0;    // dirección de lista.final
std::uintptr_t g_listCapacity = 0;  // dirección de lista.capacidad (justo después de final)
std::atomic<bool> g_active{false};
bool g_wasMenuOpen = false;

// Escribe una acción "soltar botón" en la posición "slot" del buffer.
void WriteRelease(std::uint8_t* slot, std::uint8_t button) {
    std::memset(slot, 0, kActionSize);
    // Posición actual del mouse según el propio MouseDevice (+4 x, +6 y).
    std::memcpy(slot + 0, reinterpret_cast<const void*>(g_device + 4), 2);
    std::memcpy(slot + 2, reinterpret_cast<const void*>(g_device + 6), 2);
    slot[8] = button;  // tipo de acción
    slot[9] = 0;       // dato: soltado
}

// Con el menú abierto: quitar de la lista los CLICS y la RUEDA, pero dejar
// los MOVIMIENTOS.
//
// ¿Por qué no vaciarla entera? Probamos y la cámara "saltaba" al cerrar el
// menú: si el juego no procesa los movimientos, no actualiza su registro de
// "dónde estaba el mouse", y el primer movimiento después de cerrar lleva toda
// la diferencia acumulada. Con el mouse "soltado" (sdk/mouse_grab.h) los
// movimientos ya no giran la cámara, igual que en la pausa del juego, así que
// dejarlos pasar es seguro y mantiene todo al día.
//
// La PRIMERA vez (recién abierto el menú) además agregamos "soltar izquierdo"
// y "soltar derecho": si tenías un botón apretado (rompiendo un bloque), el
// juego se entera de que lo soltaste y deja de hacerlo.
//
// Importante: NUNCA agregamos memoria a la lista (el juego y Luma usan
// administradores de memoria distintos y mezclarlos rompería todo). Solo
// movemos y escribimos dentro del espacio que la lista ya tiene reservado.
void FilterQueue(bool justOpened) {
    __try {
        auto* begin = *reinterpret_cast<std::uint8_t**>(g_listBegin);
        auto& end = *reinterpret_cast<std::uint8_t**>(g_listEnd);
        auto* capacity = *reinterpret_cast<std::uint8_t**>(g_listCapacity);
        if (begin == nullptr || end < begin) {
            return;
        }
        // "Compactar": recorrer las acciones y quedarnos solo con los
        // movimientos (tipo 0), corriéndolos hacia el principio.
        std::uint8_t* write = begin;
        for (std::uint8_t* read = begin; read + kActionSize <= end; read += kActionSize) {
            if (read[8] == kActionMove) {
                if (write != read) {
                    std::memmove(write, read, kActionSize);
                }
                write += kActionSize;
            }
        }
        if (justOpened && capacity - write >= static_cast<std::ptrdiff_t>(kActionSize * 2)) {
            WriteRelease(write, kActionLeft);
            WriteRelease(write + kActionSize, kActionRight);
            write += kActionSize * 2;
        }
        end = write;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Si la memoria no fuera la esperada, mejor no hacer nada que crashear.
    }
}

// --- Con el menú CERRADO: contar clics, limitar CPS y bloquear la rueda ------------
//
// OJO: medimos que el juego NO siempre vacía la lista justo después de
// procesarla: la llamada siguiente puede volver a ver las mismas acciones, y
// contaríamos (o limitaríamos) un clic dos veces. Comparar con "lo que vimos
// antes" no sirve (dos clics seguidos en el mismo lugar son idénticos byte a
// byte), así que MARCAMOS cada clic ya procesado en el byte +11 de la acción.
// Ese byte es "relleno": el dato termina en +9 y el campo siguiente (+12, de
// 4 bytes) tiene que empezar en una dirección múltiplo de 4, así que +10 y
// +11 no los usa nadie. El juego no lo lee; nosotros sí.
constexpr std::size_t kMarkOffset = 11;
constexpr std::uint8_t kMark = 0xA5;
constexpr std::uint8_t kActionWheel = 4;

std::atomic<bool> g_blockWheel{false};
std::atomic<int> g_cpsCap{0};          // 0 = sin límite
bool g_dropRelease[2] = {false, false};  // se descartó el "apretar": descartar también su "soltar"

void ProcessActions(bool gameHasMouse) {
    __try {
        auto* begin = *reinterpret_cast<std::uint8_t* const*>(g_listBegin);
        auto& end = *reinterpret_cast<std::uint8_t**>(g_listEnd);
        if (begin == nullptr || end < begin) {
            return;
        }
        const int cap = gameHasMouse ? g_cpsCap.load(std::memory_order_relaxed) : 0;
        const bool blockWheel = gameHasMouse && g_blockWheel.load(std::memory_order_relaxed);
        std::uint8_t* write = begin;
        for (std::uint8_t* a = begin; a + kActionSize <= end; a += kActionSize) {
            bool keep = true;
            const std::uint8_t type = a[8];
            const bool button = type == kActionLeft || type == kActionRight;
            const int index = type == kActionRight ? 1 : 0;
            if (button && a[9] != 0 && a[kMarkOffset] != kMark) {
                // Un clic nuevo. ¿Pasa el límite de CPS?
                const auto which = index == 1 ? clicks::Button::Right : clicks::Button::Left;
                if (cap > 0 && clicks::PerSecond(which) >= cap) {
                    keep = false;
                    g_dropRelease[index] = true;
                } else {
                    a[kMarkOffset] = kMark;
                    clicks::Record(which);
                }
            } else if (button && a[9] == 0 && g_dropRelease[index]) {
                keep = false;  // el "soltar" del clic que descartamos
                g_dropRelease[index] = false;
            } else if (type == kActionWheel && blockWheel) {
                keep = false;
            }
            if (keep) {
                if (write != a) {
                    std::memmove(write, a, kActionSize);
                }
                write += kActionSize;
            }
        }
        end = write;  // solo achica la lista (nunca pide memoria)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Memoria inesperada: no hacer nada antes que crashear.
    }
}

std::uintptr_t __fastcall ProcessQueueDetour(void* a, void* b, void* c) {
    hooks::ActiveCallGuard guard;
    const bool active = g_active.load(std::memory_order_relaxed);
    const bool menuOpen = active && render::overlay::IsMenuOpen();
    if (menuOpen) {
        FilterQueue(!g_wasMenuOpen);
    }
    g_wasMenuOpen = menuOpen;
    if (active && !menuOpen &&
        (clicks::Wanted() || g_blockWheel.load(std::memory_order_relaxed) || g_cpsCap.load(std::memory_order_relaxed) > 0)) {
        ProcessActions(mouse::GameMouseState() != 0);
    }
    return g_original(a, b, c);
}

}  // namespace

bool Init() {
    const ScanResult match = FindPattern(kQueuePattern);
    if (match.matches != 1) {
        log::Warn("SDK: firma de la cola del mouse no encontrada ({} coincidencias). Con el menú "
                  "abierto, el juego seguirá recibiendo clics.", match.matches);
        notify::Push(notify::Kind::Warning, "Firma no encontrada: el menú no bloqueará los clics");
        return false;
    }
    const std::uintptr_t function = match.address - kPatternOffsetInFunction;
    if (std::memcmp(reinterpret_cast<const void*>(function), kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        log::Warn("SDK: la cola del mouse no empieza donde se esperaba; no se usa.");
        return false;
    }

    // Las variables globales, leídas de las instrucciones (ver la firma):
    //   C7 05 <disp> <imm32>  -> el desplazamiento está en +5, la instrucción termina en +13
    //   48 8B 0D <disp>       -> en +16, termina en +20 (lista.final)
    //   48 8B 05 <disp>       -> en +23, termina en +27 (lista.inicio)
    g_device = ResolveRipRelative(match.address + 5, match.address + 13);
    g_listEnd = ResolveRipRelative(match.address + 16, match.address + 20);
    g_listBegin = ResolveRipRelative(match.address + 23, match.address + 27);
    g_listCapacity = g_listEnd + 8;  // std::vector = {inicio, final, capacidad}, de 8 bytes cada uno

    // Comprobación: en un std::vector, "final" va justo después de "inicio".
    if (g_listEnd != g_listBegin + 8) {
        log::Warn("SDK: la lista del mouse no tiene la forma esperada; no se usa.");
        return false;
    }

    void* target = reinterpret_cast<void*>(function);
    if (!hooks::Create(target, reinterpret_cast<void*>(&ProcessQueueDetour),
                       reinterpret_cast<void**>(&g_original), "juego: procesar cola del mouse") ||
        !hooks::Enable(target, "juego: procesar cola del mouse")) {
        return false;
    }
    g_active.store(true);
    log::Info("SDK: cola del mouse en juego+0x{:X} (MouseDevice en juego+0x{:X}, lista en +0x{:X}).",
              function - GameBase(), g_device - GameBase(), g_listBegin - g_device);
    return true;
}

bool Available() {
    return g_active.load();
}

void SetBlockWheel(bool block) {
    g_blockWheel.store(block);
}

void SetCpsCap(int cps) {
    g_cpsCap.store(cps);
}

void BeginUnload() {
    g_active.store(false);
}

}  // namespace luma::sdk::mousequeue
