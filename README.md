# Luma

Luma es un client de calidad de vida para **Minecraft Bedrock Edition en Windows** (versión GDK, `Minecraft.Windows.exe`).
Es una DLL que se carga dentro del juego oficial y le agrega un overlay, un HUD personalizable, módulos visuales y de utilidad, y optimizaciones de rendimiento medibles.

Luma **no** incluye nada que dé ventaja en multijugador: nada de killaura, fly, reach, xray, ESP, autoclicker, etc.

> **Estado:** Fases 4 y 5 terminadas (falta tu prueba en el juego).
> - **HUD:** keystrokes, FPS, CPS, reloj, marca de agua, mira personalizada, toggle sprint y toggle sneak, con un estilo propio tipo "client de PvP": cajas oscuras, texto nítido con sombra y la letra pixelada de tu juego.
> - **Editor de HUD:** arrastrar, escalar con la rueda y alinear con "imanes" a los bordes y a otros elementos.
> - **Benchmark:** FPS promedio, 1% low y frametime, exportados a CSV, con un modo "Luma en reposo" para comparar contra "sin Luma".
> - **Optimizaciones del proceso:** cuatro, cada una por separado. Más una página de consejos de Windows que solo lee y explica.
> - **Menú:** estilo "liquid glass" (vidrio que desenfoca el juego), se abre con **L** y se cierra con **L** o **Esc**. Con el menú abierto el juego no recibe ni cámara, ni clics, ni teclas.
> - **Descarga:** **Fin (End)** descarga Luma sin cerrar el juego.

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
- `LumaLauncher.exe` (el launcher con ventana)
- `LumaLauncherCli.exe` (la versión de consola, para pruebas y scripts)

Para una compilación de depuración (más lenta, pero útil para buscar errores), cambia el segundo comando por `cmake --build --preset debug`.

**Desde Visual Studio:** también puedes usar *Archivo → Abrir → Carpeta* y elegir la carpeta del proyecto. Visual Studio detecta los presets solo.

Si algo se rompe raro, borra la carpeta `build/` y vuelve a ejecutar los dos comandos. Es seguro: todo lo que hay ahí se regenera.

## Cómo usar Luma

1. Ejecuta `build\bin\Release\LumaLauncher.exe` con doble clic.
2. Pulsa **PLAY**. Si Minecraft no está abierto, el launcher lo abre, espera a que cargue e inyecta Luma. Debajo del botón va diciendo qué está haciendo, o el error y qué hacer. Cuando termina, se minimiza.
3. La tarjeta **Client Info** muestra la versión de Luma y la de Minecraft, y si el juego y Luma están funcionando ahora mismo.
4. **L** abre y cierra el menú de Luma. Limitación conocida: Luma todavía no sabe si el chat del juego está abierto, así que escribir una "l" en el chat abre el menú. Se corrige en la Fase 7. Mientras tanto puedes cambiar la tecla en *Ajustes*, por ejemplo a Insert.
5. Para quitar Luma sin cerrar el juego: pulsa **Fin (End)** con la ventana de Minecraft en primer plano. También se puede cambiar en *Ajustes*.

### El menú
- **Estilo "liquid glass":** dos paneles de vidrio que desenfocan el juego de verdad: se copia el frame y se le pasa un desenfoque gaussiano en la GPU (`client/render/gpu.cpp`). Tiene estilo oscuro y claro, con los colores de Luma (carbón `#25282B` y marfil `#E9E6DF`).
- **Barra lateral:**
  - el logo;
  - las categorías de módulos: Todos, HUD, Visual, Utilidad, Mundo y Rendimiento;
  - las páginas *Tema*, *Perfiles*, *Ajustes*, *Benchmark* y *Diagnóstico*.
- **Barra superior:** búsqueda (en todas las categorías), selector de perfil y **Editar HUD**.
- **Tarjetas de módulo:** ícono SVG propio, tecla asignada e interruptor. Al hacer clic se abre su página, con descripción, interruptor, tecla y opciones.
- **Íconos:** son "paths" SVG dibujados como vectores (`client/render/icons.h`). Los que empiezan con `F` se rellenan, como el logo de Luma.
- **Avisos (toasts), sonidos y animaciones:**
  - Los sonidos de vidrio se **sintetizan** al arrancar y suenan con XAudio2, así que no hay archivos de audio ajenos.
  - Todo se puede apagar en *Tema*.
- **Pantalla de carga** al inyectar.

### El HUD (lo que ves mientras juegas)
El estilo es común a todos los elementos y se elige en *Tema > HUD en pantalla* (`client/render/hud.h`):
- cajas cuadradas, oscuras y translúcidas;
- texto con la sombra del juego;
- opacidad, esquinas, borde y colores configurables.

**Letra "Pixel":** es la fuente pixelada que trae tu Minecraft (`Content\data\fonts\Mojangles.ttf`).
- Luma la **lee de tu instalación** al arrancar: no la incluye ni la distribuye. Si no la encuentra, usa la de Luma (Segoe UI).
- Para que se vea nítida, se carga a 8 tamaños exactos de su cuadrícula. Por eso el tamaño de los elementos va de a saltos.

Todo se mide en **píxeles de HUD**, como el juego: 1 píxel de HUD = 2 píxeles de pantalla a 1080p con tamaño 1x.

**Editor de HUD:** se abre con *Editar HUD* (`client/render/hud_editor.cpp`). El menú se oculta y cada elemento activo aparece con un contorno.

| Acción | Qué hace |
|---|---|
| Arrastrar | Mover. Se "imanta" a los bordes y al centro de la pantalla y a los bordes de los demás elementos, con una línea guía. |
| Shift mientras arrastras | Mover sin imanes. |
| Rueda | Tamaño, de 50% a 400%. |
| Clic derecho | Volver a la posición de fábrica. |
| Esc o *Listo* | Volver al menú. |

Las posiciones se guardan en el perfil como **fracciones de la pantalla** más un **ancla**: el punto del elemento que queda fijo. El ancla se elige sola según en qué tercio de la pantalla sueltes el elemento. Así, un elemento pegado a la derecha crece hacia la izquierda si su texto se alarga, y todo sirve en cualquier resolución.

**De dónde sale cada dato (sin leer memoria del juego):**

| Módulo | Dato |
|---|---|
| FPS | La medición de frames de Luma (se actualiza 4 veces por segundo para que se pueda leer). |
| CPS | Los clics de la **cola del mouse del juego** (`sdk/mouse_queue`), el mismo lugar donde los lee el juego. |
| Keystrokes | `GetAsyncKeyState`, solo con la ventana del juego en primer plano. |
| Reloj | `GetLocalTime`. |

Sobre el CPS:
- Medimos que el juego no siempre vacía esa cola tras procesarla. Para no contar un clic dos veces, Luma marca cada clic contado en un byte de **relleno** de la acción: el byte +11, que no usa nadie.
- Si la firma de la cola no se encuentra, usa los mensajes `WM_LBUTTONDOWN` de Windows como respaldo.

**Mira personalizada:** se dibuja encima de la del juego, que sigue debajo. Esconder la original requiere tocar el render del juego (Fase 8) o un paquete de recursos. Se esconde sola en los menús del juego.

**Toggle sprint / sneak** (`render/input.h`, "teclas sostenidas"):
- **Cómo mantiene la tecla:** el juego lleva la cuenta de las teclas apretadas con los mensajes de la ventana. Luma le manda un `WM_KEYDOWN` de Ctrl (o Shift) y se guarda el `WM_KEYUP` real; además, `GetAsyncKeyState` responde "apretada".
- **Cómo se alterna:** pulsando la tecla de verdad.
- **Cuándo se suelta sola:**
  - en el inventario, el chat y la pausa, porque el juego "suelta" el mouse (si no, un Shift sostenido haría que cada clic en el inventario mueva ítems);
  - al perder el foco; al volver, se aprieta de nuevo.

### Configuración y perfiles
- `%APPDATA%\Luma\config\luma.json` guarda qué perfil está activo.
- `%APPDATA%\Luma\config\profiles\<nombre>.json` guarda un perfil completo: módulos activos, sus opciones y teclas, el tema y las teclas de Luma.
- **Guardado automático:** se guarda un segundo después del último cambio, y también al descargar Luma.
- **Escritura segura:** primero se escribe a `.tmp` y después se reemplaza el archivo real, así un corte nunca lo deja a medias.
- **Archivo dañado:** si un archivo está dañado, se guarda una copia `.corrupto.json`, se usan los valores de fábrica y aparece un aviso.

### El launcher

- **Ventana propia:** Win32 + Direct3D 11 + Dear ImGui (`launcher/src/gui/`). Se mueve desde la barra de arriba, se agranda desde los bordes y tiene la sombra y las esquinas redondeadas de Windows 11.
- **Temas:** oscuro y claro, sacados de los dos colores del logo. Se cambian con el botón del sol o la luna, arriba a la derecha.
- **Idiomas:** inglés, español y portugués, se elige en *Ajustes*. Los textos están en `shared/i18n.cpp`, una línea por texto con los tres idiomas. El menú del juego va a usar los mismos textos.
- **Config:** `%APPDATA%\Luma\launcher.json` guarda el tema, el idioma y qué hacer después de PLAY.
- **Estadísticas:** Luma.dll anota en `stats.json` las sesiones (cada vez que se carga) y el tiempo jugado con Luma cargado. Lo guarda cada minuto y al descargar. El launcher lo muestra en "Your Stats".
- **Tu nombre sin cuenta:** Luma.dll guarda tu nombre de jugador (en Bedrock es tu gamertag) en `profile.json` al entrar a un mundo.
- **Cuenta de Microsoft (opcional):** *Ajustes → Cuenta*. Abre tu navegador en la página de Microsoft (OAuth 2.0 con PKCE); tu contraseña nunca pasa por Luma. Después se pide el token de Xbox Live, que trae tu gamertag y tu foto. La sesión queda en `account.dat`, cifrada con DPAPI: solo tu usuario de Windows en esta PC puede leerla. Código en `launcher/src/account.cpp`. La aplicación "Luma Launcher" está registrada en Azure; su identificador va en el código y es público por diseño.
- **Abrir con Windows:** apagado de fábrica. Si lo activas, agrega el launcher a la clave `Run` del registro de tu usuario y arranca minimizado. Al desactivarlo, se borra.
- **Rendimiento:** minimizado no dibuja nada.

Opciones de `LumaLauncherCli.exe` (la consola):

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
launcher/             LumaLauncher.exe (ventana) y LumaLauncherCli.exe (consola)
  src/flow.cpp        Lo que pasa al pulsar PLAY (abrir el juego, inyectar, confirmar)
  src/gui/            La ventana: plataforma, temas e interfaz
shared/               Lo que usan el launcher y (pronto) Luma.dll: traducciones
client/               Luma.dll
  core/               Arranque, hooks, logger, config, eventos
  render/             Overlay, editor de HUD, dibujo
  modules/            Un archivo por módulo
  sdk/                Todo lo que depende de la memoria interna del juego
  perf/               Medición, benchmark y optimizaciones
```

`sdk/` ya tiene el escáner de firmas, el control del mouse del juego (cliente y ventana) y el filtro de la cola del mouse. Se adelantaron de la Fase 6 para bloquear el mouse con el menú abierto.

## Cómo actualizar las firmas (cuando el juego se actualiza)
Si tras una actualización de Minecraft aparece el aviso *"Firma no encontrada"*, el log indica cuál falló. Para la del mouse:
1. Pon el patrón antiguo en `client/sdk/mouse_grab.cpp` y busca la función equivalente en la versión nueva, repitiendo el método de arriba:
   - registra las pilas de `ShowCursor` y `ClipCursor` al pausar;
   - desensambla las funciones que aparezcan con `dumpbin /disasm /range:...`.
2. Escribe la firma nueva con los bytes del inicio de la función. Pon `??` en todo lo que sea una dirección o un desplazamiento.
3. Comprueba que aparece **exactamente una vez** en el ejecutable.

## Cómo agregar un módulo

1. Crea `client/modules/<categoría>/<nombre>.h` y `.cpp` con una clase que herede de `luma::modules::Module`. Usa como ejemplo `modules/hud/hud_modules.cpp` (FPS, reloj...).
2. En el constructor:
   - pasa un `id` interno (sin espacios ni tildes, y que nunca cambie: es la clave en la config), el nombre visible, la descripción y la categoría;
   - si dibuja en pantalla, llama a `SetDefaultLayout(...)` con su posición de fábrica.
3. Reemplaza solo lo que necesites:
   - `HasHud()` + `OnRenderHud(frame)`: dibujar cada frame mientras está activo. Dibuja con su esquina en `frame.pos`, usa `render::hud` para el estilo y **devuelve su tamaño**. El gestor lo ubica y el editor de HUD lo mueve sin que el módulo haga nada.
   - `IsHudElement()`: devuelve `false` si dibuja pero no se debe mover, como la mira.
   - `OnEnable()` / `OnDisable()`.
   - `HasSettings()` + `DrawSettings()`: sus opciones en el menú. Devuelve `true` si algo cambió, para que se guarde.
   - `SaveSettings()` / `LoadSettings()`: sus opciones en el perfil. La posición del HUD la guarda el gestor.
   - `IconSvg()`: su ícono (un path SVG de 24x24).
4. Agrégalo a la lista en `client/modules/registry.cpp` y su `.cpp` a `client/CMakeLists.txt`.

El menú, la búsqueda, las teclas, el guardado y el editor de HUD funcionan solos. Un módulo desactivado no ejecuta código por frame, porque el render solo recorre la lista de módulos activos con HUD.

## Benchmark y optimizaciones (Fase 4)

### Benchmark (página *Benchmark*)
1. Elige nombre, duración (10-300 s) y modo.
2. Pulsa *Empezar*. El menú se cierra, hay una cuenta de 5 s (así no hay avisos en pantalla, que cuestan) y graba cada frame. Si abres el menú mientras graba, se cancela.
3. Al terminar aparece un aviso con el resultado. Los archivos quedan en `%APPDATA%\Luma\benchmarks\`:
   - `resumen.csv`: una fila por prueba, para comparar;
   - `<fecha>_<nombre>.csv`: una fila por frame, para graficar.

   Usan el separador y la coma decimal de tu Windows, así que Excel los abre bien a la primera.

Qué mide:

| Número | Significado |
|---|---|
| FPS promedio | Frames / segundos. |
| 1% low / 0.1% low | El 1% (o 0,1%) de frames más lentos, promediados y pasados a FPS. Muestran los tirones que el promedio esconde. |
| Frametime, p99, máximo | En ms. p99 = el 99% de los frames tardó menos que eso. |
| Costo de Luma | Promedio por frame. |

**Modos:**
- **Con mi HUD:** mide el juego con tu configuración, incluido lo que cuesta dibujar el HUD.
- **Luma en reposo:** durante la prueba Luma no dibuja nada. Su costo baja a ~0,001 ms por frame, así que es la referencia "sin Luma" más honesta que se puede medir desde dentro del juego. Para una medición externa de verdad sin Luma, existen herramientas como PresentMon o CapFrameX.

**Para tener números comparables** ("sin Luma / con Luma / con cada optimización"), quédate quieto en el mismo lugar y mirando lo mismo, y haz:

| # | Modo | Optimizaciones |
|---|---|---|
| 1 | Luma en reposo | ninguna (≈ sin Luma) |
| 2 | Con mi HUD | ninguna (= costo de Luma) |
| 3, 4... | Luma en reposo | **una** optimización por prueba |

Si una optimización no mejora los números, se descarta (regla del proyecto).

### Optimizaciones del proceso (categoría *Rendimiento*)
Cada una es un módulo separado: se activa sola, se mide sola y se deshace al apagarla o al descargar Luma (`client/modules/performance/process_opts.cpp`).

| Módulo | Qué hace | Cuidado |
|---|---|---|
| Prioridad del proceso | `SetPriorityClass`: "por encima de lo normal" o "alta". | "Alta" puede frenar otros programas. Nunca "tiempo real", que puede congelar el PC. |
| Timer de 1 ms | `timeBeginPeriod(1)` más la opción de Windows 11 para que se respete aunque la ventana esté tapada. Los frames pueden salir más parejos, sobre todo con límite de FPS. | Gasta un poco más de batería. |
| Sin modo eficiencia | Le prohíbe a Windows 11 poner a Minecraft en "EcoQoS" (núcleos lentos), algo que hace con ventanas en segundo plano. | — |
| Prioridad del hilo de render | Sube solo el hilo que llama a `Present`, no todo el juego. | — |

Lo que **no** hacemos, a propósito: "limpiar la RAM", trucos de registro, prioridad de tiempo real y nada que no se pueda medir y deshacer.

### Consejos de Windows
La página *Benchmark* **lee** cómo están estas opciones y explica cómo cambiarlas:
- Modo de juego.
- Programación de GPU acelerada por hardware.
- Optimizaciones para juegos en ventana.
- Plan de energía.
- GPU preferida para Minecraft (importa en portátiles con dos GPU).

Tiene botones que abren la página correspondiente de Configuración de Windows. **Luma nunca las cambia por su cuenta.**

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

### Input con el menú abierto
Con el menú abierto, el juego no debe recibir **nada**: ni cámara, ni clics, ni teclas. Hay tres capas, y hacen falta todas.

**1. Windows** (`client/render/input.cpp`). Son funciones de Windows, estables entre versiones del juego:
- **WndProc propia:** detecta la tecla del menú (por mensaje, sin revisar el teclado en cada frame), Esc y las teclas de los módulos. Con el menú abierto, el teclado y el mouse van a ImGui.
- **`PeekMessageW`:** el juego a veces mira los mensajes antes de entregarlos a la ventana. Con el menú abierto, Luma los procesa y el juego recibe un `WM_NULL`.
- **Mensajes de soltar:** los de *soltar* tecla o botón siempre pasan. El juego lleva la cuenta de las teclas apretadas con esos mensajes, y si se perdiera el "soltar W" seguiría caminando. Al abrir el menú, Luma además le avisa al juego que se soltó todo lo que estaba apretado.
- **`GetAsyncKeyState` / `GetKeyState`:** con el menú abierto, el juego ve todas las teclas sin pulsar.
- **`ClipCursor`:** con el menú abierto, el juego puede liberar el cursor, pero no encerrarlo.

**2. El cliente del juego suelta el mouse** (`client/sdk/mouse_grab.cpp`). Es lo mismo que hace la pausa: muestra el cursor y deja de tratar el mouse como "de juego". Hay dos niveles:
- **Ventana** (capa de plataforma):
  - *Agarrar*: esconde el cursor, lo encierra en el centro y pone `ventana+0x398 = 1`.
  - *Soltar*: hace lo contrario.
- **Cliente** (`ClientInstance`): su indicador `cliente+0x1F8` dice si el mouse es "de juego". Sus funciones *agarrar* (`juego+0x8BF820`) y *soltar* (`juego+0x8BFA10`) cambian ese indicador y llaman a las de la ventana. Luma usa estas, igual que otros clients.
- **Objetos sin pulsar Esc:**
  - El del cliente se captura la primera vez que el juego llama a *agarrar*, cosa que hace continuamente.
  - El de la ventana se encuentra al cargar, a partir de la variable global del WndProc del juego, en `[[contexto+0x8]+0x78]`.

**3. La cola de acciones del mouse** (`client/sdk/mouse_queue.cpp`). Es la clave para los clics:
- **Una lista central:** el juego tiene un objeto global `MouseDevice` con una lista de acciones de mouse pendientes (movimiento, clics y rueda), de 20 bytes cada una. Todo el mouse, venga de GameInput o de mensajes, termina ahí.
- **Se procesa en cada tick:** una función del juego (`juego+0x427EF0`) la recorre en cada tick.
- **Qué hace Luma:** hookea esa función y, con el menú abierto, **quita los clics y la rueda** antes de que se procesen. Al abrir el menú agrega "soltar izquierdo/derecho" para cortar acciones en curso.
- **Por qué no se vacía entera:** los movimientos se dejan pasar. Con el mouse suelto no giran la cámara, y si se borraran, la cámara saltaría al cerrar el menú porque el juego no actualizaría su posición.
- **Memoria:** nunca se le agrega memoria a la lista; solo se escribe dentro del espacio que ya tiene.

**Firmas:** todas se encuentran por escaneo (`client/sdk/scanner.cpp`). Los desplazamientos (`+0x398`, `+0x3A0`, `+0x1F8`) y las variables globales se leen de las propias instrucciones encontradas. Si alguna firma falla tras una actualización, aparece un aviso y solo se pierde esa parte del bloqueo.

**Cómo se llegó aquí.** Costó bastante, y lo dejo anotado por si sirve en otra actualización:
- Bloquear raw input, `GetCursorPos` y los métodos de GameInput **no servía**: un diagnóstico que contaba cada vía mostró que el juego no las usa para la cámara ni los clics.
- Las funciones de *agarrar/soltar* se encontraron registrando la **pila de llamadas** de `ShowCursor`/`ClipCursor` al pausar y desensamblando con `dumpbin /disasm`.
- La cola de acciones se encontró **estudiando los clients open source [Latite](https://github.com/LatiteClient/Latite) y Flarial**, para entender la idea. Las firmas de Luma son propias, sacadas del desensamblado de este juego. Las públicas de Flarial no coinciden con 1.26.63.
- Congelar `GetCursorPos` y devolver el cursor a su sitio al cerrar **causaban el salto de cámara**, porque GameInput lee la posición del cursor. Se quitaron.

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
- **Historial:** los últimos 512 frames en arreglos de tamaño fijo (un *ring buffer*), sin asignar memoria por frame. Lo usan el gráfico de *Diagnóstico* y el HUD. El benchmark guarda aparte cada frame de la prueba, en memoria reservada al empezar.
- **Resumen cada 30 s en el log:** el FPS promedio y el costo de Luma **solo de los frames con el menú cerrado** (promedio y máximo). Lo escribe el hilo principal de Luma, no el de render, así que escribir en disco no causa tirones. Así se comprueba el criterio de la Fase 2 sin abrir el menú, porque abrirlo cambia la medición.
- **Inicialización:** el overlay se inicializa una sola vez, en el primer frame. Se informa aparte en el log y no se cuenta como costo por frame.

### Camino rápido del render (Fase 3)
En cada frame, el hook de `Present` revisa tres variables atómicas:
- ¿el menú está abierto?
- ¿hay algún módulo activo que dibuje?
- ¿hay algún aviso en pantalla?

Si las tres son "no", vuelve sin tocar ImGui. Con 0 módulos activos, el costo medido fue de **~0.0009 ms por frame**; en la Fase 2 era de 0.0002-0.0005 ms. La diferencia es de menos de 1 µs, en el orden del ruido de la medición.

Costo con módulos activos (menú cerrado, 1080p, medido en el log):

| Módulos activos | Costo por frame |
|---|---|
| Marca de agua | ~0.09 ms |
| 7 elementos (keystrokes, FPS, CPS, reloj, marca de agua, mira, overlay de rendimiento) | ~0.37 ms |

Casi todo ese costo es fijo: preparar un frame de ImGui y mandar sus comandos a la GPU. Lo que cuesta cada elemento es poco. Optimizarlo más queda para la Fase 12 (por ejemplo, reutilizar la geometría del HUD entre frames si no cambió).

### Hilos y candado único
- **Tres hilos tocan el estado de Luma:**
  - el de render del juego, que dibuja;
  - el de la ventana, que recibe teclas y mouse;
  - el principal de Luma, que guarda la config y espera la tecla de descarga.
- **Candado:** todo acceso a módulos, config e ImGui pasa por `luma::StateMutex()` (`client/core/state.h`).
- **Regla para evitar bloqueos:** nunca tener el candado tomado mientras se llama a una función original del juego o de DirectX.

## Fases

Ver `CLAUDE.md` para la lista completa. Avance:

- [x] 0. Entorno
- [x] 1. Launcher + DLL mínima
- [x] 2. Overlay + medición (costo con el menú cerrado: ~0.0003 ms por frame)
- [x] 3. Sistema de módulos y config (costo con 0 módulos: ~0.0009 ms por frame)
- [x] 4. Benchmark + optimizaciones del proceso
- [x] 5. Editor de HUD + módulos sin memoria del juego (FPS, CPS, keystrokes, reloj, mira, toggle sprint/sneak)
- [ ] 6. SDK
- [ ] ...
