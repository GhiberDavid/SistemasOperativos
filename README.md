# Sistemas Operativos — Monitor de procesos con control

Monitor de procesos a pantalla completa para la terminal de **Windows**, escrito en **C++17**.
Muestra todos los procesos del sistema con su consumo de CPU y memoria, un resumen de la máquina,
y permite actuar sobre ellos: terminar, terminar por la fuerza, suspender, reanudar y cambiar la prioridad.

Incluye dos interfaces que comparten la misma lógica:

| Ejecutable | Descripción |
|---|---|
| `monitor_tui.exe` | Interfaz de terminal a pantalla completa (la entrega principal), dibujada con FTXUI |
| `monitor_gui.exe` | La misma aplicación en una ventana gráfica, dibujada con raylib |

## Cómo se obtienen los datos

Todos los datos salen directamente de la API de Windows; no se ejecuta ningún programa externo
(`tasklist`, `wmic`, PowerShell…) ni se usan librerías que entreguen procesos o %CPU ya calculados.

- **Tool Help** (`CreateToolhelp32Snapshot`): lista de procesos, PID, padre, hilos y prioridad.
- **PSAPI** (`GetProcessMemoryInfo`): memoria residente de cada proceso.
- **Contadores de kernel32** (`GetProcessTimes`, `GetSystemTimes`): tiempos de CPU.
- **ntdll** (`NtQuerySystemInformation`): estado de cada hilo.

El **%CPU se calcula en el código**: diferencia del tiempo de CPU del proceso entre dos refrescos,
dividida entre la diferencia del tiempo total del sistema en esos mismos refrescos.

## Estructura del repositorio

```
monitor/
  core/       lógica: lectura del SO (os_win.cpp), cálculos (monitor.cpp), teclas (session.cpp)
  tui/        interfaz de terminal (FTXUI)
  gui/        interfaz gráfica (raylib)
  tests/      pruebas automáticas del core con un sistema operativo simulado
  prebuilt/   ejecutables ya compilados para Windows
  README.md   documentación técnica detallada
```

## Ejecutar sin compilar

Los ejecutables de `monitor/prebuilt/` funcionan directamente. Desde `cmd` (de preferencia como Administrador):

```cmd
cd monitor\prebuilt
monitor_tui.exe
```

## Compilar (MSYS2 UCRT64)

```bash
cd monitor
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,git,raylib}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Los `.exe` quedan en `monitor/build/bin/`. La primera vez CMake descarga FTXUI (necesita internet).

## Teclas principales

| Tecla | Acción |
|---|---|
| `q` | salir |
| `↑ ↓` | mover la selección |
| `c` / `m` | ordenar por CPU / memoria |
| `/` | filtrar |
| `t` | vista de árbol padre → hijo |
| `h` | hilos del proceso con su estado |
| `g` | histórico de CPU del proceso a lo largo del tiempo |
| `k` / `K` | terminar / terminar por la fuerza |
| `s` / `r` | suspender / reanudar |
| `p` | cambiar prioridad |
| `f`, `+`, `-` | intervalo de refresco |
| `?` | ayuda |

Más detalles (decisiones de diseño, cumplimiento del enunciado y pruebas) en [`monitor/README.md`](monitor/README.md).
