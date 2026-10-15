// Controlador del monitor: une la lectura del SO, el calculo de %CPU, el historico,
// el ordenamiento/filtro/arbol y las acciones. NO sabe nada de como se dibuja.
#pragma once
#include "model.h"
#include "os_api.h"
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace mon {

class Monitor {
public:
    explicit Monitor(std::unique_ptr<OsApi> os);
    static constexpr size_t kHist = 120;   // muestras que guarda el historico de CPU (por proceso y total)

    // Lee el SO y recalcula %CPU (diferencia de tiempos entre refrescos). Seguro entre hilos.
    bool refresh(std::string* err = nullptr);
    // Copia del estado actual, ya ordenado / filtrado / en orden de arbol.
    Snapshot snapshot() const;

    // Configuracion en caliente
    void setSort(SortKey k);          SortKey sort() const;
    void setFilter(const std::string& f); std::string filter() const;
    void setTree(bool on);            bool tree() const;
    void setIntervalMs(int ms);       int intervalMs() const;   // limitado a 250..10000

    // Consultas sobre un proceso
    std::optional<ProcRow> find(uint32_t pid) const;
    std::vector<float> cpuHistory(uint32_t pid) const;          // 0..100, mas viejo primero
    bool threads(uint32_t pid, std::vector<RawThread>& out, std::string& err);

    // Acciones (devuelven mensaje claro; despues de actuar se refresca)
    ActionResult terminate(uint32_t pid, bool force);
    ActionResult suspend(uint32_t pid);
    ActionResult resume(uint32_t pid);
    ActionResult getPriority(uint32_t pid, PrioClass& cls);
    ActionResult setPriority(uint32_t pid, PrioClass cls);

private:
    struct Key {
        uint32_t pid; uint64_t creation;
        bool operator<(const Key& o) const { return pid != o.pid ? pid < o.pid : creation < o.creation; }
    };
    struct Internal { ProcRow row; uint64_t creation = 0; };

    std::unique_ptr<OsApi> os_;
    mutable std::mutex mu_;
    std::mutex refreshMu_;            // serializa las lecturas del SO (hilo de refresco vs. acciones)
    std::vector<Internal> procs_;
    Summary sum_;
    std::map<Key, uint64_t> prevCpu_;
    std::map<Key, double> lastPct_;
    std::map<Key, std::deque<float>> hist_;
    uint64_t prevSysTotal_ = 0, prevSysIdle_ = 0;
    bool havePrev_ = false;
    std::chrono::steady_clock::time_point prevTime_;
    double lastSysPct_ = 0;
    SortKey sort_ = SortKey::Cpu;
    std::string filter_;
    bool tree_ = false;
    int intervalMs_ = 2000;
};

} // namespace mon
