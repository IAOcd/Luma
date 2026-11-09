# Luma

Luma es un client de calidad de vida para **Minecraft Bedrock Edition en Windows** (versión GDK, `Minecraft.Windows.exe`).
Es una DLL que se carga dentro del juego oficial y le agrega un overlay, un HUD personalizable, módulos visuales y de utilidad, y optimizaciones de rendimiento medibles.

Luma **no** incluye nada que dé ventaja en multijugador: nada de killaura, fly, reach, xray, ESP, autoclicker, etc.

> **Estado:** Fase 1. El launcher inyecta Luma.dll, que escribe un log y se descarga con la tecla Fin (End). Todavía no hay overlay.

---

## Qué necesitas instalar

| Programa | Para qué sirve | Cómo instalarlo |
|---|---|---|
| **Visual Studio 2022** (Community es gratis) | Trae el compilador de C++, que convierte el código en `.exe` y `.dll` | En el instalador, marca la carga de trabajo **"Desarrollo para el escritorio con C++"** |
| **CMake** 3.24 o superior | Lee las "recetas" `CMakeLists.txt` y genera el proyecto de Visual Studio | `winget install Kitware.CMake`, o en el instalador de Visual Studio: "Herramientas de CMake de C++ para Windows" |
| **Git** | Guarda el historial de cambios del proyecto | `winget install Git.Git` |

Las bibliotecas externas (MinHook, Dear ImGui, nlohmann/json) **no hay que instalarlas**: CMake las descarga solo la primera vez que configuras el proyecto. Por eso esa primera vez necesitas internet.

## Cómo compilar

Abre una terminal (PowerShell) en la carpeta del proyecto y ejecuta:

```
cmake --preset x64
cmake --build --preset release
```

- El primer comando **configura**: descarga las dependencias y genera el proyecto de Visual Studio en `build/`.
- El segundo **compila** en modo Release (optimizado).

El resultado queda en `build/bin/Release/`:

- `Luma.dll`
- `LumaLauncher.exe`

Para una compilación de depuración (más lenta, pero útil para buscar errores), cambia el segundo comando por `cmake --build --preset debug`.

**Desde Visual Studio:** también puedes usar *Archivo → Abrir → Carpeta* y elegir la carpeta del proyecto. Visual Studio detecta los presets solo.

Si algo se rompe raro, borra la carpeta `build/` y vuelve a ejecutar los dos comandos. Es seguro: todo lo que hay ahí se regenera.

## Cómo usar Luma

1. Abre Minecraft Bedrock (versión GDK).
2. Ejecuta `build\bin\Release\LumaLauncher.exe` (con doble clic o desde la terminal).
3. El launcher muestra cada paso con `[OK]`, o un `[ERROR]` que explica qué pasó y qué hacer.
4. Para quitar Luma sin cerrar el juego: pulsa **Fin (End)** con la ventana de Minecraft en primer plano.

Opciones del launcher:

| Opción | Qué hace |
|---|---|
| `--no-pause` | No espera a que pulses Enter al terminar (útil para scripts) |
| `--skip-gdk-check` | Salta la comprobación de versión GDK. Úsala solo si la detección falla y estás seguro de tener la versión GDK |

**Logs:** cada vez que Luma se carga crea un archivo nuevo en `%APPDATA%\Luma\logs\` y conserva los últimos 10. Para abrir la carpeta, pega `%APPDATA%\Luma\logs` en la barra de direcciones del Explorador de archivos.

**Consola de depuración:** si usas la compilación Debug (`build\bin\Debug\`), se abre además una ventana con el log en vivo. Su botón de cerrar está desactivado a propósito: cerrar esa ventana cerraría el juego entero. Desaparece sola al descargar Luma.

## Estructura del proyecto

```
CMakeLists.txt        Receta principal: opciones comunes y subcarpetas
CMakePresets.json     Atajos de configuración (x64, release, debug)
cmake/
  Dependencies.cmake  Qué bibliotecas externas se descargan y en qué versión
launcher/             LumaLauncher.exe: encuentra el juego e inyecta Luma.dll
client/               Luma.dll
  core/               Arranque, hooks, logger, config, eventos
  render/             Overlay, editor de HUD, dibujo
  modules/            Un archivo por módulo
  sdk/                Todo lo que depende de la memoria interna del juego
  perf/               Medición, benchmark y optimizaciones
```

## Decisiones técnicas

### Dependencias con FetchContent (y no vcpkg)
`FetchContent` viene incluido en CMake, así que no hay que instalar nada extra.
Cada biblioteca tiene una **versión fija** en `cmake/Dependencies.cmake`, para que una actualización de terceros nunca rompa la compilación por sorpresa.

| Biblioteca | Versión | Licencia | Uso |
|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | 1.3.4 | BSD-2-Clause | Interceptar funciones (hooks) |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.5 | MIT | Interfaz del menú y HUD |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT | Archivos de configuración |

### Runtime de C++ estático (`/MT`)
El código de soporte de C++ queda dentro de `Luma.dll`. Así la DLL no depende de archivos externos ni choca con la versión del runtime que use el juego.

### Solo x64
Minecraft GDK es un proceso de 64 bits, y una DLL de 32 bits no puede cargarse en él. CMake rechaza otras arquitecturas con un error claro.

### Inyección: `LoadLibraryW` + `CreateRemoteThread`
Es el método más simple y conocido: el launcher escribe la ruta de `Luma.dll` en la memoria del juego y crea ahí un hilo que la carga. No modifica ningún archivo del juego. Está explicado paso a paso en `launcher/src/injector.cpp`.

### Detección de la versión GDK
- La UWP antigua corre dentro de un *AppContainer* (el sandbox de las apps de la Store) y GDK no. Si el proceso está en un AppContainer, el launcher lo rechaza.
- Los juegos GDK traen `MicrosoftGame.config` junto al ejecutable. Si ese archivo falta, el launcher también lo rechaza.
- **No** se usa el nombre del paquete: la versión GDK sigue llamándose `Microsoft.MinecraftUWP` por compatibilidad.
- Ruta de instalación: por defecto `C:\XboxGames\Minecraft for Windows\Content\`. Desde dentro del juego esa misma carpeta se ve como `C:\Program Files\WindowsApps\Microsoft.MinecraftUWP_...`, porque así monta Windows los juegos GDK. Es normal que el log muestre esa segunda ruta.

### Ciclo de vida de la DLL
`DllMain` solo crea un hilo y todo el trabajo se hace en ese hilo (`client/core/luma.cpp`). Los pasos son:
- **Arranque** (`Startup`).
- **Espera** a la tecla Fin, revisándola cada 50 ms.
- **Apagado** (`Shutdown`), que deshace todo en orden inverso.
- **Salida** con `FreeLibraryAndExitThread`, que quita la DLL de la memoria y termina el hilo en un solo paso.

Si el juego se cierra con Luma cargado, Luma no hace nada en `DLL_PROCESS_DETACH`: tocar candados en ese momento podría congelar el cierre.

### API gráfica del overlay
Se decide y se documenta en la Fase 2.

## Fases

Ver `CLAUDE.md` para la lista completa. Avance:

- [x] 0. Entorno
- [x] 1. Launcher + DLL mínima
- [ ] 2. Overlay + medición
- [ ] ...
