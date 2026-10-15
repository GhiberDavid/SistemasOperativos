// Tipos compartidos por el core y las dos interfaces (TUI y GUI).
// Este archivo NO depende de Windows, FTXUI ni raylib.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mon {

// ---------- Datos crudos tal como los entrega el sistema operativo ----------
struct RawProc {
    uint32_t pid = 0, ppid = 0;
    std::string name;          // nombre del ejecutable
    std::string cmd;           // linea de comando (o ruta si no se pudo leer)
    std::string user;          // usuario dueno ("?" si no hay permiso)
    uint64_t creation = 0;     // instante de creacion (evita confundir PIDs reutilizados)
    uint64_t cpu100ns = 0;     // tiempo de CPU acumulado (kernel + user), unidades de 100 ns
    uint64_t rss = 0;          // memoria residente (working set) en bytes
    uint32_t threads = 0;
    int basePrio = 0;          // prioridad base (0..31 en Windows)
    char state = 'S';          // R corriendo, S durmiendo, D esperando E/S, T detenido, Z terminado sin recoger
    uint32_t runnableThreads = 0; // hilos listos/corriendo (para la carga promedio)
    bool denied = false;       // no se pudo consultar por falta de permisos
};

struct RawSystem {
    uint64_t idle = 0, kernel = 0, user = 0; // GetSystemTimes (kernel incluye idle), unidades de 100 ns
    uint64_t memTotal = 0, memAvail = 0;     // bytes
    uint64_t uptimeMs = 0;
};

struct RawThread {
    uint32_t tid = 0;
    int basePrio = 0, prio = 0;
    char code = 'S';           // R, S, D, T, Z
    std::string state;         // texto legible
    std::string wait;          // razon de espera
    uint64_t cpu100ns = 0;
};

// Clases de prioridad (se mapean a las clases de Windows)
enum class PrioClass { Idle, BelowNormal, Normal, AboveNormal, High, Realtime };
const char* prioName(PrioClass p);
int prioBase(PrioClass p);     // prioridad base aproximada de la clase

struct ActionResult {
    bool ok = false;
    std::string message;
};

// ---------- Datos ya calculados, listos para dibujar ----------
struct ProcRow {
    uint32_t pid = 0, ppid = 0;
    std::string user, name, cmd;
    char state = 'S';
    double cpu = 0;            // %CPU del proceso (0..100 de toda la maquina)
    double mem = 0;            // %MEM
    uint64_t rss = 0;
    uint32_t threads = 0;
    int prio = 0;
    uint64_t cpuTime = 0;      // 100 ns
    bool denied = false;
    int depth = 0;             // nivel en el arbol (0 en la tabla plana)
    std::string treePrefix;    // "├─ " etc. para la vista de arbol
};

struct Summary {
    int total = 0, running = 0, stopped = 0, zombie = 0, blocked = 0, sleeping = 0;
    double load1 = 0, load5 = 0, load15 = 0;
    uint64_t memTotal = 0, memUsed = 0, memAvail = 0;
    double cpuTotal = 0;       // %CPU global
    std::vector<float> cpuHistory; // historico del CPU global (0..100)
    uint64_t uptimeMs = 0;
    uint64_t refreshes = 0;
};

struct Snapshot {
    Summary sum;
    std::vector<ProcRow> rows;  // ya ordenadas / filtradas / en orden de arbol
    int hiddenByFilter = 0;
};

enum class SortKey { Cpu, Mem, Pid, Name };
const char* sortName(SortKey k);

// Estadisticas de un historico de %CPU (vista de historico, tecla g)
struct CpuStats { float now = 0, avg = 0, max = 0; size_t samples = 0; };
CpuStats cpuStats(const std::vector<float>& h);
float niceScale(float maxValue);           // tope del eje Y: 10, 25, 50 o 100 %

// Utilidades de formato (para que ambas interfaces muestren lo mismo)
std::string fmtBytes(uint64_t b);
std::string fmtCpuTime(uint64_t t100ns);   // mm:ss.cc o h:mm:ss
std::string fmtUptime(uint64_t ms);

} // namespace mon
