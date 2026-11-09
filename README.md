# Luma

Luma es un client de calidad de vida para **Minecraft Bedrock Edition en Windows** (versión GDK, `Minecraft.Windows.exe`).
Es una DLL que se carga dentro del juego oficial y le agrega un overlay, un HUD personalizable, módulos visuales y de utilidad, y optimizaciones de rendimiento medibles.

Luma **no** incluye nada que dé ventaja en multijugador: nada de killaura, fly, reach, xray, ESP, autoclicker, etc.

> **Estado:** Fase 0 (entorno). El proyecto compila, pero todavía no hace nada.

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

### API gráfica del overlay
Se decide y se documenta en la Fase 2.

## Fases

Ver `CLAUDE.md` para la lista completa. Avance:

- [x] 0. Entorno
- [ ] 1. Launcher + DLL mínima
- [ ] 2. Overlay + medición
- [ ] ...
