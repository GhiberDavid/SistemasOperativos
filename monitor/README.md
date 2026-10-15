# Monitor de procesos con control (Windows, C++)

Un solo **core** con toda la lógica y **dos interfaces** que lo comparten:

| Ejecutable | Qué es | Librería (solo dibujo) |
|---|---|---|
| `monitor_tui.exe` | **TUI a pantalla completa en la terminal** (la entrega del enunciado) | FTXUI (MIT) |
| `monitor_gui.exe` | Misma app en una ventana gráfica (extra) | raylib (zlib) |

```
core/
  os_api.h      interfaz con el SO (lo único que toca la API nativa)
  os_win.cpp    Windows: Tool Help + PSAPI + kernel32 (+ NtQuerySystemInformation para el estado de hilos)
  monitor.*     lectura, %CPU por diferencia de tiempos, histórico, orden, filtro, árbol, acciones
  session.*     estado de la aplicación y manejo de teclas (idéntico en TUI y GUI)
  model.*       tipos y formato
tui/main.cpp    solo dibuja con FTXUI
gui/main.cpp    solo dibuja con raylib
tests/          pruebas del core con un backend simulado (corren en Linux, no hacen falta en Windows)
```

## Compilar (MSYS2 UCRT64)

```bash
cd /c/proyectos/monitor
bash build_msys2.sh          # instala gcc, cmake, ninja, git y raylib (pacman) y compila
```
o a mano:
```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,git,raylib}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
La primera vez CMake descarga FTXUI v6.1.9 de GitHub (necesita internet y git). Los `.exe` quedan en
`build/bin/` enlazados de forma estática: se pueden ejecutar desde **PowerShell / Windows Terminal** sin tocar el PATH.

> Ejecuta el TUI en **Windows Terminal** (o PowerShell moderno), no en la consola antigua.
> Para poder terminar/suspender/cambiar prioridad de procesos de otros usuarios o del sistema, abre la terminal **como Administrador**.

Si no puedes compilar, en `prebuilt/` hay ejecutables ya compilados (cross-compilados con llvm-mingw; ver "Qué se probó").

## Teclas (iguales en TUI y GUI)

| Tecla | Acción |
|---|---|
| `q` | salir (restaura eco, cursor y prompt) |
| ↑ ↓ PgUp PgDn Home End, rueda | mover la selección |
| `c` `m` `n` `i` | ordenar por CPU, memoria, nombre, PID (CPU y memoria: el mayor primero) |
| `/` | filtrar por nombre, comando, usuario o PID (en vivo; Enter acepta, Esc cancela) |
| `t` | vista de árbol padre → hijo |
| `h` | hilos del proceso seleccionado, con su estado (`h`/Esc vuelve) |
| `g` | histórico de CPU del proceso seleccionado a lo largo del tiempo, más el del CPU total (↑↓ cambia de proceso; `g`/Esc vuelve) |
| `k` | terminar (pide confirmación) |
| `K` | terminar por la fuerza (confirmación aparte) |
| `s` / `r` | suspender / reanudar |
| `p` | ver y cambiar la prioridad (menú) |
| `f`, `+`, `-` | intervalo de refresco: escribir milisegundos (250–10000) o ±250 ms |
| `?` | ayuda |

Colores: **rojo** = terminado sin recoger (Z), **amarillo** = bloqueado esperando E/S (D), **magenta** = suspendido (T), gris = sin permiso para consultar.

## Cumplimiento del enunciado

| # | Requisito | Dónde / cómo |
|---|---|---|
| 1 | Tabla con PID, padre, usuario, estado, %CPU, %MEM, RSS, hilos, prioridad, tiempo CPU, comando | `tui/main.cpp renderTable`, `gui/main.cpp` |
| 2 | Resumen: totales, corriendo, detenidos, zombis, carga, memoria | `Monitor::refresh` → `Summary`; cabecera de ambas UI |
| 3 | %CPU propio por diferencia de tiempos | `monitor.cpp`: `100·(Δcpu_proceso)/(Δ(kernel+user) del sistema)` entre dos refrescos, con `GetProcessTimes` y `GetSystemTimes`. Es el mismo criterio del Administrador de tareas (0–100 % de **toda** la máquina) |
| 4 | Orden por CPU / memoria | teclas `c`, `m` |
| 5 | Árbol padre → hijo | tecla `t` (`Monitor::snapshot`, usa PID/PPID y fecha de creación para no confundir PIDs reutilizados) |
| 6 | Hilos con estado | tecla `h` (`OsApi::threads`) |
| 7 | Histórico de CPU por proceso | tecla `g`: vista grande con el histórico del proceso seleccionado (últimas 120 muestras, promedio y máximo) y el del CPU total; también un gráfico pequeño en la cabecera si la ventana es ancha |
| 8 | Resaltado de zombis y bloqueados en E/S | rojo / amarillo |
| 9 | Terminar, forzar (confirmación aparte), suspender, reanudar | `k`, `K`, `s`, `r` → `OsApi` |
| 10 | Ver y cambiar prioridad | `p` (`GetPriorityClass` / `SetPriorityClass`), columna PRI = prioridad base |
| 11 | TUI a pantalla completa, sin parpadeo, restaura la terminal | FTXUI `ScreenInteractive::Fullscreen()`: pantalla alternativa, alto/ancho reales, redimensionable, dibuja cada cuadro de una vez |
| 12 | Intervalo configurable | `f`, `+`, `-` |
| 13 | Cada acción con su tecla, en caliente | tabla de teclas |
| 14 | Confirmación indicando proceso y acción | línea de mensaje: «OK Terminar -> chrome.exe (PID 1234): …» |
| 15 | Mensaje claro si el proceso ya no existe o no hay permiso | `winErr()` en `os_win.cpp`; la app nunca se cae por eso |

## Cómo se obtienen los datos (sin herramientas externas ni librerías de procesos)

* Lista de procesos, PID, padre, hilos, prioridad base: **Tool Help** (`CreateToolhelp32Snapshot`).
* CPU de cada proceso: `GetProcessTimes`; del sistema: `GetSystemTimes`; memoria: `GetProcessMemoryInfo` (PSAPI) y `GlobalMemoryStatusEx`.
* Usuario: `OpenProcessToken` + `LookupAccountSidW`. Comando: PEB del proceso vía `NtQueryInformationProcess` + `ReadProcessMemory` (si no hay permiso se muestra la ruta del ejecutable).
* Estado de los hilos (Tool Help no lo da): `NtQuerySystemInformation(SystemProcessInformation)`. También sirve de respaldo de CPU/memoria para procesos protegidos que no se pueden abrir.
* Acciones: `TerminateProcess`, `NtSuspendProcess` / `NtResumeProcess`, `SetPriorityClass`.
* No se ejecuta `tasklist`, `wmic`, PowerShell, `cmd` ni nada externo.

## Decisiones por ser Windows (conviene mencionarlas en el informe)

* **Zombis**: Windows no tiene zombis, pero sí el equivalente: un proceso que ya terminó y cuyo objeto sigue vivo porque otro proceso mantiene un handle abierto. Se detecta con 0 hilos / código de salida ≠ `STILL_ACTIVE` y se muestra como `Z`.
* **Bloqueado esperando E/S (`D`)**: proceso con algún hilo esperando por paginación (`PageIn`/`WrPageIn`), lo más cercano a «espera de disco» que expone el planificador.
* **Estados**: `R` algún hilo listo/corriendo, `S` durmiendo, `D` E/S, `T` todos los hilos suspendidos, `Z` terminado sin recoger.
* **Carga promedio**: Windows no la tiene. Se calcula como en Unix, con medias móviles exponenciales (1, 5 y 15 min) del número de hilos listos/corriendo más procesos en `D`. Arranca en 0 al abrir el programa y sube con el tiempo.
* **«Terminar»** pide cierre educado: `WM_CLOSE` a las ventanas del proceso o, si no tiene ventanas, un hilo remoto que llama a `ExitProcess`. **«Forzar»** usa `TerminateProcess`. Si el proceso no acepta cierre educado, el mensaje sugiere usar `K`.
* **System Idle Process (PID 0)** no se lista como proceso (su «CPU» es el tiempo ocioso); sí se usa para el CPU total.
* PID 4 (`System`), `Registry`, etc. no se pueden abrir: aparecen en gris y marcados «sin permiso»; sus contadores salen igualmente.

## Qué se probó

* **Core + manejo de teclas**: pruebas automáticas (`tests/core_test.cpp`, `-DMONITOR_FAKE_OS=ON` en Linux) con un backend simulado: %CPU exacto (25 %, 12.5 %…), orden, filtro, árbol, hilos, terminar/suspender/reanudar/prioridad, procesos inexistentes y sin permiso. Todas pasan.
* **TUI**: ejecutada en un pseudo-terminal con el backend simulado (tabla, árbol, hilos, filtro, confirmaciones, prioridad, ayuda, terminal de 80×20 y de 130 columnas).
* **GUI**: renderizada sin pantalla con el backend por software de raylib (capturas de tabla, árbol e hilos).
* **Código de Windows (`os_win.cpp`)**: compila y enlaza sin advertencias con llvm-mingw (x86_64, UCRT). **No pude ejecutarlo en una máquina Windows real**: prueba en tu equipo este checklist antes de entregar:
  1. Abre `notepad.exe`, búscalo con `/notepad`, `s` (se pone en `T`), `r` (continúa), `p` → Alta, `k` → `s` (se cierra).
  2. `K` sobre otro Notepad (forzar). 3. `h` sobre el navegador (hilos con estado). 4. `t` (árbol). 5. Redimensiona la terminal y sal con `q`: el prompt, el cursor y el eco deben quedar normales.
  6. Un `while(1)` en otra ventana → su %CPU debe ser ≈ 100/núcleos y coincidir con el Administrador de tareas.

## Pendiente de confirmar con el profesor

* Que se permite una librería **solo de dibujo** (FTXUI / raylib): el enunciado solo prohíbe librerías de procesos o %CPU ya calculados.
* Que `NtQuerySystemInformation` (API nativa del sistema) vale para el estado de los hilos.
* Cómo quiere ver «zombis» y «carga promedio» en Windows (ver arriba).
