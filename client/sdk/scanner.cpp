// =============================================================================
//  scanner.cpp - Implementación del escáner de firmas
// =============================================================================
#include "sdk/scanner.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace luma::sdk {

namespace {

struct Section {
    const std::uint8_t* start = nullptr;
    std::size_t size = 0;
};

// Encuentra la sección .text (el código) leyendo la cabecera PE del juego,
// que Windows deja en memoria al principio del módulo.
Section CodeSection() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            return Section{base + section->VirtualAddress, section->Misc.VirtualSize};
        }
    }
    return {};
}

// Convierte "48 8B ?? 05" en bytes y una máscara (true = comparar).
bool Parse(std::string_view pattern, std::vector<std::uint8_t>& bytes, std::vector<bool>& mask) {
    std::size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ' ') {
            ++i;
            continue;
        }
        if (pattern[i] == '?') {
            bytes.push_back(0);
            mask.push_back(false);
            while (i < pattern.size() && pattern[i] == '?') {
                ++i;
            }
            continue;
        }
        if (i + 1 >= pattern.size()) {
            return false;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = hex(pattern[i]);
        const int lo = hex(pattern[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        mask.push_back(true);
        i += 2;
    }
    return !bytes.empty() && mask.front();  // el primer byte no puede ser comodín
}

}  // namespace

std::uintptr_t GameBase() {
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

ScanResult FindPattern(std::string_view pattern) {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;
    ScanResult result;
    const Section code = CodeSection();
    if (!Parse(pattern, bytes, mask) || code.start == nullptr || code.size < bytes.size()) {
        return result;
    }

    const std::uint8_t* const end = code.start + code.size - bytes.size() + 1;
    const std::uint8_t* p = code.start;
    while (p < end) {
        // memchr busca el primer byte muy rápido; solo donde coincide
        // comparamos el resto de la firma.
        p = static_cast<const std::uint8_t*>(std::memchr(p, bytes[0], static_cast<std::size_t>(end - p)));
        if (p == nullptr) {
            break;
        }
        bool ok = true;
        for (std::size_t j = 1; j < bytes.size(); ++j) {
            if (mask[j] && p[j] != bytes[j]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            if (result.matches == 0) {
                result.address = reinterpret_cast<std::uintptr_t>(p);
            }
            ++result.matches;
        }
        ++p;
    }
    return result;
}

}  // namespace luma::sdk
