# Luma

Luma es un client de calidad de vida para **Minecraft Bedrock Edition en Windows** (versión GDK, `Minecraft.Windows.exe`).
Es una DLL que se carga dentro del juego oficial y le agrega un overlay, un HUD personalizable, módulos visuales y de utilidad, y optimizaciones de rendimiento medibles.

Luma **no** incluye nada que dé ventaja en multijugador: nada de killaura, fly, reach, xray, ESP, autoclicker, etc.

> **Estado:** Fase 2. Overlay DX12 con el menú de Luma (tecla **L**) y una pestaña de Rendimiento que muestra el frametime y el costo de Luma. Fin (End) descarga Luma sin cerrar el juego.

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
4. **L** abre y cierra el menú de Luma. Limitación conocida: Luma todavía no sabe si el chat del juego está abierto, así que escribir una "l" en el chat abre el menú. Se corrige en la Fase 7.
5. Para quitar Luma sin cerrar el juego: pulsa **Fin (End)** con la ventana de Minecraft en primer plano.

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

Las carpetas `modules/` y `sdk/` todavía están vacías; cada una tiene un README que dice en qué fase se llena.

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

### API gráfica del overlay: DirectX 12
Minecraft GDK carga `d3d12.dll` y dibuja con DirectX 12, salvo en algunas PCs con GPUs antiguas, donde usa DirectX 11. Luma dibuja su overlay con DX12 usando los backends oficiales de ImGui (`imgui_impl_dx12` + `imgui_impl_win32`). Si detecta que el juego usa otra API, desactiva solo el overlay, lo anota en el log y el juego sigue normal. El soporte DX11 (y una opción para forzarlo) queda para la Fase 4, y se va a medir con el benchmark.

**Cómo se engancha** (`client/render/dx12_hooks.cpp`):
1. Luma crea por un instante una swapchain DX12 de prueba sobre una ventana invisible y lee de su *vtable* las direcciones de `Present`, `Present1`, `ResizeBuffers` y `ResizeBuffers1`. También lee `ExecuteCommandLists` de una cola de comandos de prueba.
2. Las posiciones dentro de la vtable las fija el ABI de COM y nunca cambian. Además son funciones de Windows, no del juego, así que **no dependen de la versión de Minecraft** y no necesitan escaneo de firmas.
3. Con MinHook se interceptan esas funciones:
   - **`ExecuteCommandLists`:** captura la cola *directa* del juego, que es la que hace falta en DX12 para mandar trabajo a la GPU.
   - **`Present`:** dibuja el menú encima del frame.
   - **`ResizeBuffers`:** suelta y recrea los recursos cuando cambia el tamaño de la ventana.

**Costo con el menú cerrado:** el hook de `Present` sale enseguida, después de leer un par de variables. ImGui no se ejecuta.

**Input** (`client/render/input.cpp`): Luma reemplaza la *WndProc* de la ventana del juego. La tecla del menú se detecta por mensaje, sin revisar el teclado en cada frame. Con el menú abierto, los mensajes de teclado y mouse van a ImGui y no al juego, e ImGui dibuja su propio cursor.

Al probarlo descubrimos que eso no alcanzaba: la cámara seguía moviéndose y el cursor quedaba clavado. Revisando con `dumpbin /imports` qué funciones de Windows importa `Minecraft.Windows.exe`, aparecieron dos responsables:
- **`GetRawInputData`:** el juego lee el movimiento del mouse como *raw input* directamente desde su cola de mensajes, antes de que llegue a la WndProc. Con el menú abierto, el hook le devuelve un mouse "quieto" (movimiento 0, sin clics).
- **`ClipCursor`:** dentro de un mundo, el juego encierra el cursor. Con el menú abierto, el hook ignora esos pedidos y libera el cursor. Al cerrar el menú, restaura lo último que pidió el juego.

Las dos son funciones de Windows, así que no dependen de la versión del juego.

**GameInput** (`client/render/gameinput_block.cpp`): el log mostró que el juego nunca llama a `GetRawInputData`, así que el mouse tenía que llegarle por otro lado. El juego carga `GameInputRedist.dll`, y en su ejecutable aparece el IID de `IGameInput` versión 0, la que describe el `GameInput.h` del Windows SDK. Luma hookea `IGameInputReading::GetMouseState`, `GetKeyCount` y `GetKeyState` para ocultar mouse y teclado con el menú abierto. Las direcciones salen de la vtable de una lectura propia.

> **Problema conocido (pendiente):** aun con todo lo anterior, con el menú abierto el juego sigue moviendo la cámara y recibiendo clics. Falta descubrir por qué camino le llega el mouse. Ver la Fase 3.

**Descarga segura**, en este orden:
1. Los hooks dejan de tocar el estado de Luma.
2. MinHook restaura las funciones originales.
3. Se espera a que ningún hilo del juego siga dentro de código de Luma.
4. Se restaura la WndProc original.
5. Se espera a la GPU y se liberan ImGui y DX12.

**Asserts de ImGui:** en Debug, un error interno de ImGui cerraría el juego. `client/render/imgui_config.h` lo redirige al log.

### Medición de rendimiento (`client/perf/`)
- **Reloj:** `QueryPerformanceCounter`, que tiene una resolución de ~100 ns y cuesta ~20-30 ns leerlo.
- **Frametime:** el tiempo entre dos llamadas a `Present`.
- **Costo de Luma:** el tiempo desde que entra el hook de `Present` hasta que termina el código de Luma, sin contar el `Present` original del juego. El propio registro de la medición (unas decenas de ns) queda fuera. Los hooks de `ExecuteCommandLists` y `GetRawInputData` no se miden porque, con el menú cerrado, solo leen una variable.
- **Historial:** los últimos 512 frames en arreglos de tamaño fijo (un *ring buffer*), sin asignar memoria por frame. Lo usa el gráfico de la pestaña Rendimiento. El "1% low" sale del percentil 99 del frametime.
- **Resumen cada 30 s en el log:** el FPS promedio y el costo de Luma **solo de los frames con el menú cerrado** (promedio y máximo). Lo escribe el hilo principal de Luma, no el de render, así que escribir en disco no causa tirones. Así se comprueba el criterio de la Fase 2 sin abrir el menú, porque abrirlo cambia la medición.
- **Inicialización:** el overlay se inicializa una sola vez, en el primer frame. Se informa aparte en el log y no se cuenta como costo por frame.

## Fases

Ver `CLAUDE.md` para la lista completa. Avance:

- [x] 0. Entorno
- [x] 1. Launcher + DLL mínima
- [x] 2. Overlay + medición (costo con el menú cerrado: ~0.0003 ms por frame)
- [ ] 3. Sistema de módulos y config
- [ ] ...
