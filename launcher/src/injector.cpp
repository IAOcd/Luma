// =============================================================================
//  injector.cpp - Inyección por LoadLibraryW + CreateRemoteThread
// =============================================================================
//  Idea general: Windows tiene una función, LoadLibraryW, que hace que el
//  proceso que la llama cargue una DLL. Nosotros queremos que el JUEGO la
//  llame (no nuestro launcher). Para eso:
//
//    1. Abrimos el proceso del juego con permisos para escribir en su memoria
//       y crear hilos.
//    2. Reservamos un pedacito de memoria DENTRO del juego y escribimos ahí la
//       ruta de Luma.dll (el juego no puede leer la memoria del launcher).
//    3. Creamos un hilo dentro del juego que ejecuta LoadLibraryW(ruta).
//       Funciona porque kernel32.dll, donde vive LoadLibraryW, se carga en la
//       misma dirección en todos los procesos durante una misma sesión de
//       Windows: la dirección que vemos en el launcher sirve en el juego.
//    4. Esperamos a que ese hilo termine y liberamos la memoria del paso 2.
//
//  Es el método de inyección más simple y conocido. No modifica archivos del
//  juego: todo ocurre en memoria y desaparece al cerrar el juego.
// =============================================================================
#include "injector.h"

#include "process.h"

namespace luma::launcher {

namespace {

// Tiempo máximo esperando a que la DLL termine de cargar.
constexpr DWORD kLoadTimeoutMs = 10'000;

// Libera la memoria reservada dentro del juego al salir de la función, pase
// lo que pase (misma idea RAII que UniqueHandle).
class RemoteMemory {
public:
    RemoteMemory(HANDLE process, void* address) : process_(process), address_(address) {}
    ~RemoteMemory() {
        if (address_ != nullptr) {
            VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
        }
    }
    RemoteMemory(const RemoteMemory&) = delete;
    RemoteMemory& operator=(const RemoteMemory&) = delete;

    void* Get() const { return address_; }
    // Renunciar a liberarla (ver el caso de timeout más abajo).
    void Leak() { address_ = nullptr; }

private:
    HANDLE process_;
    void* address_;
};

InjectResult Fail(std::string message) {
    return InjectResult{false, std::move(message)};
}

}  // namespace

InjectResult InjectDll(DWORD pid, const std::filesystem::path& dllPath) {
    // --- Paso 1: abrir el proceso ----------------------------------------------
    // Pedimos solo los permisos que vamos a usar.
    constexpr DWORD kAccess = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ | PROCESS_QUERY_INFORMATION;
    UniqueHandle process(OpenProcess(kAccess, FALSE, pid));
    if (!process.Valid()) {
        const DWORD error = GetLastError();
        std::string message = "No se pudo abrir el proceso del juego: " + ErrorMessage(error);
        if (error == ERROR_ACCESS_DENIED) {
            message +=
                "\n  Si abriste Minecraft como administrador, abre también el launcher como "
                "administrador.";
        }
        return Fail(message);
    }

    // --- Paso 2: escribir la ruta de la DLL en la memoria del juego ---------
    const std::wstring& path = dllPath.native();
    const SIZE_T pathBytes = (path.size() + 1) * sizeof(wchar_t);  // +1 por el '\0' final

    RemoteMemory remotePath(process.Get(),
                            VirtualAllocEx(process.Get(), nullptr, pathBytes,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (remotePath.Get() == nullptr) {
        return Fail("No se pudo reservar memoria en el juego: " + ErrorMessage(GetLastError()));
    }
    if (!WriteProcessMemory(process.Get(), remotePath.Get(), path.c_str(), pathBytes, nullptr)) {
        return Fail("No se pudo escribir en la memoria del juego: " +
                    ErrorMessage(GetLastError()));
    }

    // --- Paso 3: crear el hilo que llama a LoadLibraryW ------------------------
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto* loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    if (loadLibrary == nullptr) {
        return Fail("No se encontró LoadLibraryW: " + ErrorMessage(GetLastError()));
    }

    UniqueHandle thread(CreateRemoteThread(process.Get(), nullptr, 0, loadLibrary,
                                           remotePath.Get(), 0, nullptr));
    if (!thread.Valid()) {
        return Fail("No se pudo crear el hilo de carga en el juego: " +
                    ErrorMessage(GetLastError()) +
                    "\n  Un antivirus puede estar bloqueando la inyección.");
    }

    // --- Paso 4: esperar el resultado -------------------------------------------
    const DWORD wait = WaitForSingleObject(thread.Get(), kLoadTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        // El hilo sigue usando la memoria con la ruta. Si la liberáramos ahora,
        // el juego podría leer memoria inválida y crashear. Preferimos "perder"
        // esos pocos bytes (se liberan al cerrar el juego).
        remotePath.Leak();
        return Fail("La carga de Luma.dll tardó demasiado (más de 10 s).");
    }

    // El código de salida del hilo es lo que devolvió LoadLibraryW: la
    // dirección de la DLL cargada, o 0 si falló. (Solo vemos 32 de sus 64 bits,
    // pero alcanza para saber si es 0 o no.)
    DWORD exitCode = 0;
    GetExitCodeThread(thread.Get(), &exitCode);
    if (exitCode == 0) {
        return Fail(
            "El juego no pudo cargar Luma.dll (LoadLibraryW devolvió error).\n"
            "  Causas posibles: la DLL está dañada, falta algún archivo o un antivirus la bloqueó.\n"
            "  Revisa el log en %APPDATA%\\Luma\\logs por si alcanzó a escribir algo.");
    }

    // Comprobación final: preguntarle a Windows si Luma.dll aparece en la lista
    // de módulos del juego.
    const std::optional<bool> loaded = IsModuleLoaded(pid, dllPath.filename().native());
    if (loaded.has_value() && !*loaded) {
        return Fail("La carga pareció funcionar, pero Luma.dll no aparece en el juego.");
    }

    return InjectResult{true, {}};
}

}  // namespace luma::launcher
