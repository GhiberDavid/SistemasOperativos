// Interfaz con el sistema operativo. Todo lo que toca la API nativa vive detras de esta clase.
#pragma once
#include "model.h"
#include <memory>

namespace mon {

class OsApi {
public:
    virtual ~OsApi() = default;
    // Lee todos los procesos y los contadores del sistema.
    virtual bool snapshot(std::vector<RawProc>& procs, RawSystem& sys, std::string& err) = 0;
    // Hilos de un proceso con su estado.
    virtual bool threads(uint32_t pid, std::vector<RawThread>& out, std::string& err) = 0;
    // Acciones sobre un proceso
    virtual ActionResult terminate(uint32_t pid, bool force) = 0;
    virtual ActionResult suspend(uint32_t pid) = 0;
    virtual ActionResult resume(uint32_t pid) = 0;
    virtual ActionResult getPriority(uint32_t pid, PrioClass& cls) = 0;
    virtual ActionResult setPriority(uint32_t pid, PrioClass cls) = 0;
};

// Fabrica: devuelve la implementacion de Windows (o la simulada si se compila con MONITOR_FAKE_OS)
std::unique_ptr<OsApi> makeOsApi();

} // namespace mon
