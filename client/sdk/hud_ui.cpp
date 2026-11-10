// =============================================================================
//  hud_ui.cpp - Coordenadas y scoreboard del juego (ver hud_ui.h)
// =============================================================================
//  REGLA DE ORO (aprendida con un crash): NUNCA escribir en un control que
//  guardamos de un frame anterior. El juego destruye y vuelve a crear partes de
//  la interfaz (el scoreboard, cuando cambian los puntos), y esa memoria puede
//  pasar a ser de otra cosa: escribir ahí rompe el juego.
//
//  Por eso no guardamos punteros: guardamos el CAMINO desde la raíz ("el hijo
//  3 del hijo 0 del hijo 5..."). Cada frame el juego nos da la raíz actual,
//  seguimos el camino con punteros recién leídos y confirmamos el nombre. Si
//  no coincide, buscamos de nuevo (cada tanto, no cada frame).
// =============================================================================
#include "sdk/hud_ui.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/logger.h"
#include "sdk/memory.h"
#include "core/hooks.h"
#include "sdk/scanner.h"

namespace luma::sdk::hudui {

namespace {

constexpr std::uintptr_t kControlPosition = 0x10;  // Vec2 (en pantalla)
constexpr std::uintptr_t kControlName = 0x20;      // std::string
constexpr std::uintptr_t kControlRelative = 0x40;  // Vec2 (relativa al padre)
constexpr std::uintptr_t kControlBounds = 0x48;    // Vec2 (tamaño)
constexpr std::uintptr_t kControlChildren = 0x98;  // vector<shared_ptr<UIControl>>

constexpr float kHiddenOffset = -4000.0f;  // "fuera de la pantalla"
constexpr int kMaxDepth = 18;
constexpr int kSearchEvery = 120;           // frames entre búsquedas si no lo encontramos

std::atomic<bool> g_hideCoords{false};
std::atomic<bool> g_boardEnabled{false};
std::atomic<float> g_boardX{0.0f};
std::atomic<float> g_boardY{0.0f};
std::atomic<float> g_boardW{0.0f};
std::atomic<float> g_boardH{0.0f};
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_restoreDone{false};

// Un camino desde la raíz: el índice del hijo en cada nivel.
struct Path {
    int index[kMaxDepth]{};
    int length = -1;  // -1 = no lo conocemos
    char name[32]{};  // el nombre que tiene que tener al final
};

// --- El enganche de UIControl::updatePos ------------------------------------------
// El juego no calcula la posición de un control en el momento: lo marca como
// "pendiente" (bit 0 de +0x18) y la calcula cuando la necesita para dibujar,
// con una función que hace  posición = posición del padre + relativa.
// (Encontrada con un punto de vigilancia de memoria en 1.26.63.)
// La enganchamos: cuando el juego calcula la del contenedor del scoreboard, la
// corregimos al instante, antes de que se dibuje nada. Los hijos calculan la
// suya desde la del padre, así que lo siguen solos (también las copias nuevas
// que crea el servidor). Firma: el final de la función, que es único:
//     movsd xmm0,[rsi+40h] / addps xmm6,xmm0 / movlps [rsi+10h],xmm6 / and byte [rsi+18h],0FEh
constexpr const char* kUpdatePosTail = "F2 0F 10 46 40 0F 58 F0 0F 13 76 10 80 66 18 FE";
constexpr std::uintptr_t kUpdatePosTailOffset = 0xB8;  // desde el comienzo de la función
constexpr std::uintptr_t kControlFlags = 0x18;         // bit 0 = posición pendiente

using UpdatePosFn = void(__fastcall*)(void* control);
UpdatePosFn g_originalUpdatePos = nullptr;
std::atomic<bool> g_hookReady{false};
// Qué control corregir y a dónde (los pone el hilo de la interfaz cada frame,
// antes de dibujar; el puntero solo se COMPARA, nunca se usa para leer).
std::atomic<std::uintptr_t> g_fixControl{0};
std::atomic<float> g_fixX{0.0f};
std::atomic<float> g_fixY{0.0f};

void __fastcall UpdatePosDetour(void* control) {
    hooks::ActiveCallGuard guard;
    g_originalUpdatePos(control);
    const std::uintptr_t c = reinterpret_cast<std::uintptr_t>(control);
    if (c != 0 && c == g_fixControl.load(std::memory_order_relaxed)) {
        const float pos[2] = {g_fixX.load(std::memory_order_relaxed), g_fixY.load(std::memory_order_relaxed)};
        SafeWrite(c + kControlPosition, pos, sizeof(pos));
    }
}

// Estado del hilo de la interfaz (solo valores, nada de punteros guardados).
Path g_coordsPath;
Path g_boardPath;
int g_framesSinceSearch = 1000;
bool g_coordsHidden = false;
bool g_boardMoved = false;

bool NameIs(std::uintptr_t control, const char* name) {
    char text[40];
    return SafeReadString(control + kControlName, text, sizeof(text)) && std::strcmp(text, name) == 0;
}

bool Children(std::uintptr_t control, std::uintptr_t& first, std::size_t& count) {
    std::uintptr_t range[2]{};
    if (!SafeRead(control + kControlChildren, range) || range[1] < range[0] || (range[1] - range[0]) / 16 > 512) {
        return false;
    }
    first = range[0];
    count = static_cast<std::size_t>((range[1] - range[0]) / 16);  // shared_ptr = 2 punteros
    return true;
}

std::uintptr_t Child(std::uintptr_t control, std::size_t i) {
    std::uintptr_t first = 0;
    std::size_t count = 0;
    std::uintptr_t child = 0;
    if (!Children(control, first, count) || i >= count || !SafeRead(first + i * 16, child) || !LooksLikePointer(child)) return 0;
    return child;
}

template <typename F>
void ForChildren(std::uintptr_t control, F&& f) {
    std::uintptr_t first = 0;
    std::size_t count = 0;
    if (!Children(control, first, count)) return;
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t child = 0;
        if (SafeRead(first + i * 16, child) && LooksLikePointer(child)) f(child);
    }
}

// Busca un control por nombre y anota el camino hasta él.
bool FindPath(std::uintptr_t control, const char* name, int depth, int& budget, Path& path) {
    if (depth >= kMaxDepth || --budget < 0 || !LooksLikePointer(control)) return false;
    if (NameIs(control, name)) {
        path.length = depth;
        std::snprintf(path.name, sizeof(path.name), "%s", name);
        return true;
    }
    std::uintptr_t first = 0;
    std::size_t count = 0;
    if (!Children(control, first, count)) return false;
    for (std::size_t i = 0; i < count; ++i) {
        std::uintptr_t child = 0;
        if (!SafeRead(first + i * 16, child) || !LooksLikePointer(child)) continue;
        path.index[depth] = static_cast<int>(i);
        if (FindPath(child, name, depth + 1, budget, path)) return true;
    }
    return false;
}

// Sigue el camino desde la raíz de ESTE frame. 0 si ya no lleva al control.
std::uintptr_t Resolve(std::uintptr_t root, const Path& path, std::uintptr_t* parentOut = nullptr) {
    if (path.length < 0) return 0;
    std::uintptr_t c = root;
    std::uintptr_t parent = 0;
    for (int d = 0; d < path.length; ++d) {
        parent = c;
        c = Child(c, static_cast<std::size_t>(path.index[d]));
        if (c == 0) return 0;
    }
    if (!NameIs(c, path.name)) return 0;
    if (parentOut != nullptr) *parentOut = parent;
    return c;
}

// Como Resolve, pero devuelve el control de CADA nivel (chain[0] = raíz).
bool ResolveChain(std::uintptr_t root, const Path& path, std::uintptr_t* chain) {
    if (path.length < 0) return false;
    chain[0] = root;
    for (int d = 0; d < path.length; ++d) {
        chain[d + 1] = Child(chain[d], static_cast<std::size_t>(path.index[d]));
        if (chain[d + 1] == 0) return false;
    }
    return NameIs(chain[path.length], path.name);
}

std::size_t ChildCount(std::uintptr_t control) {
    std::uintptr_t first = 0;
    std::size_t count = 0;
    return Children(control, first, count) ? count : 0;
}

// Pone las posiciones en pantalla de los hijos como las calcula el juego:
// posición del hijo = posición del padre + su posición relativa (+0x40).
void PlaceChildren(std::uintptr_t control, int depth, int& budget) {
    if (depth > 12) return;
    float parent[2]{};
    if (!SafeRead(control + kControlPosition, parent) || !std::isfinite(parent[0]) || !std::isfinite(parent[1])) return;
    ForChildren(control, [&](std::uintptr_t child) {
        if (--budget < 0) return;
        float rel[2]{};
        if (SafeRead(child + kControlRelative, rel) && std::isfinite(rel[0]) && std::isfinite(rel[1]) &&
            std::fabs(rel[0]) < 10000.0f && std::fabs(rel[1]) < 10000.0f) {
            const float pos[2] = {parent[0] + rel[0], parent[1] + rel[1]};
            SafeWrite(child + kControlPosition, pos, sizeof(pos));
        }
        PlaceChildren(child, depth + 1, budget);
    });
}

// Pone un control (y todo lo de adentro) con su esquina en (x, y).
void PlaceAt(std::uintptr_t control, float x, float y) {
    const float pos[2] = {x, y};
    SafeWrite(control + kControlPosition, pos, sizeof(pos));
    int budget = 400;
    PlaceChildren(control, 0, budget);
}

// Dónde lo pondría el juego: posición del padre + su relativa.
bool GamePosition(std::uintptr_t control, std::uintptr_t parent, float& x, float& y) {
    float parentPos[2]{};
    float rel[2]{};
    if (parent == 0 || !SafeRead(parent + kControlPosition, parentPos) || !SafeRead(control + kControlRelative, rel) ||
        !std::isfinite(parentPos[0]) || !std::isfinite(parentPos[1]) || !std::isfinite(rel[0]) || !std::isfinite(rel[1])) {
        return false;
    }
    x = parentPos[0] + rel[0];
    y = parentPos[1] + rel[1];
    return true;
}

// Marca un control y todo lo de adentro como "posición pendiente" (lo mismo
// que hace el juego): la próxima vez que se dibuje, se recalcula.
void MarkDirty(std::uintptr_t control, int depth, int& budget) {
    if (depth > 12 || --budget < 0) return;
    std::uint8_t flags = 0;
    if (SafeRead(control + kControlFlags, flags)) {
        flags |= 1;
        SafeWrite(control + kControlFlags, &flags, sizeof(flags));
    }
    ForChildren(control, [&](std::uintptr_t child) { MarkDirty(child, depth + 1, budget); });
}

void MarkDirty(std::uintptr_t control) {
    int budget = 400;
    MarkDirty(control, 0, budget);
}

// El rectángulo que ocupa un control con todo lo de adentro que tenga tamaño.
struct Box {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    bool Valid() const { return x1 > x0 && y1 > y0; }
};
void Measure(std::uintptr_t control, Box& box, int depth, int& budget) {
    if (depth > 12 || --budget < 0) return;
    float pos[2]{};
    float size[2]{};
    if (SafeRead(control + kControlPosition, pos) && SafeRead(control + kControlBounds, size) && std::isfinite(pos[0]) &&
        std::isfinite(pos[1]) && size[0] > 0.5f && size[1] > 0.5f && size[0] < 5000.0f && size[1] < 5000.0f) {
        box.x0 = std::fmin(box.x0, pos[0]);
        box.y0 = std::fmin(box.y0, pos[1]);
        box.x1 = std::fmax(box.x1, pos[0] + size[0]);
        box.y1 = std::fmax(box.y1, pos[1] + size[1]);
    }
    ForChildren(control, [&](std::uintptr_t child) { Measure(child, box, depth + 1, budget); });
}

Box BoxOf(std::uintptr_t control) {
    Box box;
    int budget = 400;
    Measure(control, box, 0, budget);
    return box;
}

// --- Coordenadas ---------------------------------------------------------------------
void UpdateCoords(std::uintptr_t root, bool hide) {
    std::uintptr_t parent = 0;
    const std::uintptr_t coords = Resolve(root, g_coordsPath, &parent);
    if (coords == 0) return;
    float pos[2]{};
    float gx = 0.0f, gy = 0.0f;
    if (!SafeRead(coords + kControlPosition, pos) || !std::isfinite(pos[1]) || !GamePosition(coords, parent, gx, gy)) return;
    const bool offscreen = pos[1] < kHiddenOffset * 0.5f;
    if (hide && !offscreen) {
        PlaceAt(coords, gx, gy + kHiddenOffset);  // (también si el juego lo volvió a acomodar)
        g_coordsHidden = true;
    } else if (!hide && offscreen) {
        PlaceAt(coords, gx, gy);
        g_coordsHidden = false;
    }
}

// --- Scoreboard --------------------------------------------------------------------
// Algunos servidores dibujan su scoreboard dentro del área de los TÍTULOS y lo
// vuelven a crear varias veces por segundo (está animado). Mover el control
// del scoreboard no sirve: la copia nueva aparece en el lugar original. Por
// eso movemos el CONTENEDOR estable más alto que tiene solo al scoreboard
// adentro (subimos mientras el padre tenga un solo hijo). Le cambiamos la
// posición Y la relativa: así las copias nuevas se crean ya en el lugar nuevo.
int g_boardMoveDepth = -1;          // qué nivel del camino movemos
std::uintptr_t g_movedControl = 0;  // SOLO para comparar (nunca se lee ni se escribe con él)
float g_origRel[2]{};               // la relativa que le puso el juego

int ContainerDepth(const std::uintptr_t* chain, int length) {
    int d = length;
    while (d > 1 && ChildCount(chain[d - 1]) == 1) --d;
    return d;
}

void UpdateBoard(std::uintptr_t root, bool enabled, float tx, float ty) {
    std::uintptr_t chain[kMaxDepth + 1]{};
    if (!ResolveChain(root, g_boardPath, chain) || g_boardMoveDepth < 1 || g_boardMoveDepth > g_boardPath.length) {
        g_fixControl.store(0, std::memory_order_relaxed);
        g_boardW.store(0.0f, std::memory_order_relaxed);
        g_boardH.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t board = chain[g_boardPath.length];
    const std::uintptr_t mover = chain[g_boardMoveDepth];
    const std::uintptr_t parent = chain[g_boardMoveDepth - 1];
    float parentPos[2]{};
    float rel[2]{};
    float pos[2]{};
    if (!SafeRead(parent + kControlPosition, parentPos) || !SafeRead(mover + kControlRelative, rel) ||
        !SafeRead(mover + kControlPosition, pos) || !std::isfinite(parentPos[0]) || !std::isfinite(rel[0]) || !std::isfinite(pos[0])) {
        return;
    }
    // ¿Es otro control (el juego lo volvió a crear)? Entonces su relativa es la del juego.
    if (mover != g_movedControl) {
        g_movedControl = mover;
        g_origRel[0] = rel[0];
        g_origRel[1] = rel[1];
        g_boardMoved = false;
    }
    if (!enabled) {
        if (g_boardMoved) {  // devolverlo
            if (g_hookReady.load(std::memory_order_relaxed)) {
                g_fixControl.store(0, std::memory_order_relaxed);
                MarkDirty(mover);  // el juego lo recalcula en su lugar
            } else {
                SafeWrite(mover + kControlRelative, g_origRel, sizeof(g_origRel));
                PlaceAt(mover, parentPos[0] + g_origRel[0], parentPos[1] + g_origRel[1]);
            }
            g_boardMoved = false;
        }
    }
    // Lo que ocupa: el tamaño del propio scoreboard si lo tiene (los paquetes
    // de los servidores suelen traer partes invisibles adentro que agrandan
    // la suma); si no tiene tamaño (el del juego), lo que ocupa lo de adentro.
    Box box;
    float boardPos[2]{};
    float boardSize[2]{};
    if (SafeRead(board + kControlPosition, boardPos) && SafeRead(board + kControlBounds, boardSize) && std::isfinite(boardPos[0]) &&
        std::isfinite(boardPos[1]) && boardSize[0] > 4.0f && boardSize[1] > 4.0f && boardSize[0] < 2000.0f && boardSize[1] < 2000.0f) {
        box.x0 = boardPos[0];
        box.y0 = boardPos[1];
        box.x1 = boardPos[0] + boardSize[0];
        box.y1 = boardPos[1] + boardSize[1];
    } else {
        box = BoxOf(board);
    }
    if (!box.Valid()) {
        g_boardW.store(0.0f, std::memory_order_relaxed);
        g_boardH.store(0.0f, std::memory_order_relaxed);
        return;
    }
    g_boardW.store(box.x1 - box.x0, std::memory_order_relaxed);
    g_boardH.store(box.y1 - box.y0, std::memory_order_relaxed);
    if (!enabled) return;
    // Lo que se ve tiene que quedar con su esquina en (tx, ty).
    const float newX = pos[0] + (tx - box.x0);
    const float newY = pos[1] + (ty - box.y0);
    if (g_hookReady.load(std::memory_order_relaxed)) {
        // Con el enganche: solo le decimos a dónde. Si cambió, lo marcamos
        // pendiente para que el juego lo recalcule (y pase por el enganche).
        const bool changed = g_fixControl.load(std::memory_order_relaxed) != mover ||
                             std::fabs(g_fixX.load(std::memory_order_relaxed) - newX) > 0.25f ||
                             std::fabs(g_fixY.load(std::memory_order_relaxed) - newY) > 0.25f;
        g_fixX.store(newX, std::memory_order_relaxed);
        g_fixY.store(newY, std::memory_order_relaxed);
        g_fixControl.store(mover, std::memory_order_relaxed);
        if (changed) MarkDirty(mover);
        g_boardMoved = true;
        return;
    }
    // Sin el enganche (no se encontró la función): moverlo nosotros (puede titilar).
    if (std::fabs(newX - pos[0]) < 0.25f && std::fabs(newY - pos[1]) < 0.25f && g_boardMoved) return;  // ya está
    const float newRel[2] = {newX - parentPos[0], newY - parentPos[1]};
    SafeWrite(mover + kControlRelative, newRel, sizeof(newRel));
    PlaceAt(mover, newX, newY);
    g_boardMoved = true;
}

// El scoreboard que se ve: el del juego o el de un paquete del servidor (el más grande).
void SearchBoard(std::uintptr_t root) {
    static const char* const kNames[] = {"sidebar", "hcf_sidebar", "scoreboard_sidebar"};
    Path best;
    float bestArea = -1.0f;
    int bestDepth = -1;
    // Puede haber varios con el mismo nombre (el del juego vacío y el del
    // servidor): probamos el primero de cada nombre y además buscamos
    // saltándonos los vacíos.
    for (const char* name : kNames) {
        int skip = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            Path path;
            int budget = 4000;
            int seen = 0;
            // FindPath devuelve el primero; para encontrar el siguiente, buscamos
            // de nuevo ignorando los "skip" primeros.
            struct Finder {
                static bool Find(std::uintptr_t c, const char* n, int depth, int& budget, Path& p, int& seen, int skip) {
                    if (depth >= kMaxDepth || --budget < 0 || !LooksLikePointer(c)) return false;
                    if (NameIs(c, n)) {
                        if (seen++ == skip) {
                            p.length = depth;
                            std::snprintf(p.name, sizeof(p.name), "%s", n);
                            return true;
                        }
                    }
                    std::uintptr_t first = 0;
                    std::size_t count = 0;
                    if (!Children(c, first, count)) return false;
                    for (std::size_t i = 0; i < count; ++i) {
                        std::uintptr_t child = 0;
                        if (!SafeRead(first + i * 16, child) || !LooksLikePointer(child)) continue;
                        p.index[depth] = static_cast<int>(i);
                        if (Find(child, n, depth + 1, budget, p, seen, skip)) return true;
                    }
                    return false;
                }
            };
            if (!Finder::Find(root, name, 0, budget, path, seen, skip)) break;
            ++skip;
            std::uintptr_t chain[kMaxDepth + 1]{};
            if (!ResolveChain(root, path, chain)) continue;
            const Box bx = BoxOf(chain[path.length]);
            const float area = bx.Valid() ? (bx.x1 - bx.x0) * (bx.y1 - bx.y0) : 0.0f;
            if (area > bestArea) {
                best = path;
                bestArea = area;
                bestDepth = ContainerDepth(chain, path.length);
            }
        }
    }
    g_boardPath = best;
    g_boardMoveDepth = bestDepth;
    // (g_movedControl no se borra: si es el mismo contenedor, sigue valiendo la
    // relativa original que guardamos.)
    if (best.length >= 0) {
        std::uintptr_t chain[kMaxDepth + 1]{};
        char mover[40]{};
        if (ResolveChain(root, best, chain) && bestDepth >= 1) SafeReadString(chain[bestDepth] + kControlName, mover, sizeof(mover));
        log::Info("SDK: scoreboard \"{}\" (nivel {}), se mueve con \"{}\".", best.name, best.length, mover);
    }
}

}  // namespace

void OnHudScreen(std::uintptr_t root) {
    const bool unloading = g_unloading.load(std::memory_order_relaxed);
    const bool hide = !unloading && g_hideCoords.load(std::memory_order_relaxed);
    const bool board = !unloading && g_boardEnabled.load(std::memory_order_relaxed);
    if (!hide && !board && !g_coordsHidden && !g_boardMoved) {  // nada que hacer: no recorrer el árbol
        if (unloading) g_restoreDone.store(true);
        return;
    }
    // ¿Siguen ahí? Si no, buscar de nuevo (cada tanto).
    const bool lostCoords = (hide || g_coordsHidden) && Resolve(root, g_coordsPath) == 0;
    const bool lostBoard = (board || g_boardMoved) && Resolve(root, g_boardPath) == 0;
    if ((lostCoords || lostBoard) && ++g_framesSinceSearch > kSearchEvery) {
        g_framesSinceSearch = 0;
        if (lostCoords) {
            Path path;
            int budget = 3000;
            g_coordsPath = FindPath(root, "player_position", 0, budget, path) ? path : Path{};
            if (g_coordsPath.length < 0) g_coordsHidden = false;
        }
        if (lostBoard) {
            SearchBoard(root);
            g_boardMoved = false;  // el nuevo lo acomodó el juego
        }
        static bool logged = false;
        if (!logged && (g_coordsPath.length >= 0 || g_boardPath.length >= 0)) {
            logged = true;
            log::Info("SDK: interfaz del HUD: coordenadas {}, scoreboard {} (\"{}\").",
                      g_coordsPath.length >= 0 ? "encontradas" : "no encontradas",
                      g_boardPath.length >= 0 ? "encontrado" : "no encontrado", g_boardPath.name);
        }
    }
    UpdateCoords(root, hide);
    UpdateBoard(root, board, g_boardX.load(std::memory_order_relaxed), g_boardY.load(std::memory_order_relaxed));
    if (unloading) g_restoreDone.store(true);
}

void BeforeHudRender(std::uintptr_t root) {
    if (g_unloading.load(std::memory_order_relaxed) || !g_boardEnabled.load(std::memory_order_relaxed)) return;
    UpdateBoard(root, true, g_boardX.load(std::memory_order_relaxed), g_boardY.load(std::memory_order_relaxed));
}

bool Init() {
    const ScanResult tail = FindPattern(kUpdatePosTail);
    if (tail.matches != 1) {
        log::Warn("SDK: no se encontró UIControl::updatePos ({} coincidencias): el scoreboard movible puede titilar.",
                  tail.matches);
        return false;
    }
    const std::uintptr_t start = tail.address - kUpdatePosTailOffset;
    // Comprobar que ahí empieza la función: push rbp / push rsi / push rdi / sub rsp,50h
    static const std::uint8_t kStart[] = {0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x50};
    std::uint8_t bytes[sizeof(kStart)]{};
    if (!SafeCopy(start, bytes, sizeof(bytes)) || std::memcmp(bytes, kStart, sizeof(kStart)) != 0) {
        log::Warn("SDK: UIControl::updatePos no empieza como se esperaba: el scoreboard movible puede titilar.");
        return false;
    }
    void* target = reinterpret_cast<void*>(start);
    if (!hooks::Create(target, reinterpret_cast<void*>(&UpdatePosDetour), reinterpret_cast<void**>(&g_originalUpdatePos),
                       "juego: posición de un control (UIControl::updatePos)") ||
        !hooks::Enable(target, "juego: posición de un control (UIControl::updatePos)")) {
        return false;
    }
    g_hookReady.store(true);
    log::Info("SDK: UIControl::updatePos en juego+0x{:X}.", start - GameBase());
    return true;
}

void SetHideCoords(bool hide) {
    g_hideCoords.store(hide, std::memory_order_relaxed);
}

void SetScoreboard(bool enabled, float x, float y) {
    g_boardX.store(x, std::memory_order_relaxed);
    g_boardY.store(y, std::memory_order_relaxed);
    g_boardEnabled.store(enabled, std::memory_order_relaxed);
}

float ScoreboardWidth() {
    return g_boardW.load(std::memory_order_relaxed);
}

float ScoreboardHeight() {
    return g_boardH.load(std::memory_order_relaxed);
}

void BeginUnload() {
    // Devolver todo a su lugar lo hace el hilo de la interfaz (el único que
    // puede tocarla sin riesgo): le avisamos y esperamos un momento a que pase
    // un frame del HUD. Si no hay HUD en pantalla, no hay nada que devolver.
    g_restoreDone.store(false);
    g_fixControl.store(0, std::memory_order_relaxed);
    g_unloading.store(true);
    for (int i = 0; i < 25 && !g_restoreDone.load(); ++i) {
        Sleep(10);
    }
}

}  // namespace luma::sdk::hudui
