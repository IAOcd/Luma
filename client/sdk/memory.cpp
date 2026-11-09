// =============================================================================
//  memory.cpp - Lecturas seguras (ver memory.h)
// =============================================================================
#include "sdk/memory.h"

#include <windows.h>

namespace luma::sdk {

bool SafeCopy(std::uintptr_t address, void* out, std::size_t size) {
    if (address < 0x10000) {
        return false;
    }
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadString(std::uintptr_t address, char* out, std::size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    out[0] = '\0';
    struct MsvcString {
        union {
            char buffer[16];
            std::uintptr_t pointer;
        };
        std::uint64_t size;
        std::uint64_t capacity;
    } s{};
    if (!SafeRead(address, s) || s.size > 4096 || s.capacity < s.size) {
        return false;
    }
    const std::size_t n = s.size < outSize - 1 ? static_cast<std::size_t>(s.size) : outSize - 1;
    const bool ok = s.capacity < 16 ? (std::memcpy(out, s.buffer, n), true) : SafeCopy(s.pointer, out, n);
    out[ok ? n : 0] = '\0';
    return ok;
}

}  // namespace luma::sdk
