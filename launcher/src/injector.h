// =============================================================================
//  injector.h - Inyectar una DLL en otro proceso
// =============================================================================
#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace luma::launcher {

struct InjectResult {
    bool ok = false;
    std::string error;  // Explicación para el usuario si ok == false.
};

// Hace que el proceso "pid" cargue la DLL que está en "dllPath".
// Ver injector.cpp para la explicación paso a paso.
InjectResult InjectDll(DWORD pid, const std::filesystem::path& dllPath);

}  // namespace luma::launcher
