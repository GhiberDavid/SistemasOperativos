// Backend SIMULADO, solo para probar el core y las interfaces en Linux/macOS (CMake -DMONITOR_FAKE_OS=ON).
// No se usa en la entrega real para Windows.
#include "../core/os_api.h"
#include <algorithm>
#include <map>

namespace mon {
namespace {

struct FP { RawProc p; double coreUse = 0; bool suspended = false; char saved = 0; PrioClass cls = PrioClass::Normal; };

class FakeOs : public OsApi {
public:
    FakeOs() {
        add(4, 0, "System", 0.02, 4, 120, 'S', "SYSTEM");
        add(100, 4, "smss.exe", 0, 2, 4, 'S', "SYSTEM");
        add(200, 100, "csrss.exe", 0.01, 12, 9, 'S', "SYSTEM");
        add(300, 4, "explorer.exe", 0.10, 60, 150, 'S', "ghiber");
        add(310, 300, "chrome.exe", 1.00, 45, 710, 'R', "ghiber");     // 1 nucleo completo
        add(311, 310, "chrome.exe", 0.50, 20, 320, 'R', "ghiber");
        add(312, 310, "chrome.exe", 0.00, 12, 538, 'S', "ghiber");
        add(320, 300, "code.exe", 0.25, 30, 480, 'S', "ghiber");
        add(330, 300, "disco.exe", 0.00, 3, 20, 'D', "ghiber");        // bloqueado en E/S
        add(340, 300, "huerfano.exe", 0.00, 0, 0, 'Z', "ghiber");      // terminado sin recoger
        add(350, 4, "protegido.exe", 0.00, 5, 30, 'S', "?");
        fp_[350].p.denied = true;
    }
    bool snapshot(std::vector<RawProc>& procs, RawSystem& sys, std::string&) override {
        ++tick_;
        const uint64_t sec = 10000000ull;                  // 1 s en unidades de 100 ns
        uint64_t total = 4 * sec;                          // 4 nucleos, 1 s por muestra
        double busy = 0;
        for (auto& kv : fp_) {
            auto& f = kv.second;
            double use = f.suspended ? 0 : f.coreUse;
            f.p.cpu100ns += uint64_t(use * sec);
            busy += use;
        }
        sysTotal_ += total; sysIdle_ += uint64_t((4 - busy) * sec);
        sys.kernel = sysTotal_; sys.user = 0; sys.idle = sysIdle_;
        sys.memTotal = 16ull << 30; sys.memAvail = 10ull << 30; sys.uptimeMs = tick_ * 1000;
        procs.clear();
        for (auto& kv : fp_) procs.push_back(kv.second.p);
        RawProc idle; idle.pid = 0; idle.name = "System Idle Process"; idle.creation = 1; idle.threads = 4;
        idle.cpu100ns = sysIdle_; procs.push_back(idle);
        return true;
    }
    bool threads(uint32_t pid, std::vector<RawThread>& out, std::string& err) override {
        auto it = fp_.find(pid);
        if (it == fp_.end()) { err = "El proceso PID " + std::to_string(pid) + " ya no existe"; return false; }
        out.clear();
        for (uint32_t i = 0; i < it->second.p.threads; ++i) {
            RawThread t; t.tid = pid * 10 + i; t.basePrio = it->second.p.basePrio; t.prio = t.basePrio;
            t.code = it->second.p.state; t.state = t.code == 'R' ? "Running" : "Waiting";
            t.wait = t.code == 'S' ? "UserRequest" : t.code == 'D' ? "PageIn" : t.code == 'T' ? "Suspended" : "";
            out.push_back(t);
        }
        if (out.empty()) err = "El proceso no tiene hilos (terminado sin recoger)";
        return true;
    }
    ActionResult terminate(uint32_t pid, bool force) override {
        ActionResult r;
        auto it = fp_.find(pid);
        if (it == fp_.end()) { r.message = "El proceso PID " + std::to_string(pid) + " ya no existe"; return r; }
        if (it->second.p.denied) { r.message = "Acceso denegado sobre PID " + std::to_string(pid); return r; }
        fp_.erase(it); r.ok = true;
        r.message = std::string(force ? "terminado por la fuerza" : "terminado con salida ordenada");
        return r;
    }
    ActionResult suspend(uint32_t pid) override { return susp(pid, true); }
    ActionResult resume(uint32_t pid) override { return susp(pid, false); }
    ActionResult getPriority(uint32_t pid, PrioClass& c) override {
        ActionResult r; auto it = fp_.find(pid);
        if (it == fp_.end()) { r.message = "El proceso PID " + std::to_string(pid) + " ya no existe"; return r; }
        if (it->second.p.denied) { r.message = "Acceso denegado sobre PID " + std::to_string(pid); return r; }
        c = it->second.cls; r.ok = true; r.message = std::string("prioridad ") + prioName(c); return r;
    }
    ActionResult setPriority(uint32_t pid, PrioClass c) override {
        ActionResult r; auto it = fp_.find(pid);
        if (it == fp_.end()) { r.message = "El proceso PID " + std::to_string(pid) + " ya no existe"; return r; }
        if (it->second.p.denied) { r.message = "Acceso denegado sobre PID " + std::to_string(pid); return r; }
        it->second.cls = c; it->second.p.basePrio = prioBase(c); r.ok = true;
        r.message = std::string("prioridad cambiada a ") + prioName(c); return r;
    }
private:
    ActionResult susp(uint32_t pid, bool s) {
        ActionResult r; auto it = fp_.find(pid);
        if (it == fp_.end()) { r.message = "El proceso PID " + std::to_string(pid) + " ya no existe"; return r; }
        if (it->second.p.denied) { r.message = "Acceso denegado sobre PID " + std::to_string(pid); return r; }
        auto& f = it->second; f.suspended = s;
        if (s) { f.saved = f.p.state; f.p.state = 'T'; } else f.p.state = f.saved ? f.saved : 'S';
        r.ok = true; r.message = s ? "suspendido" : "reanudado"; return r;
    }
    void add(uint32_t pid, uint32_t ppid, const char* name, double use, uint32_t th, uint64_t mb, char st, const char* user) {
        FP f; f.p.pid = pid; f.p.ppid = ppid; f.p.name = name; f.p.cmd = std::string("C:\\fake\\") + name;
        f.p.user = user; f.p.creation = 100 + pid; f.p.threads = th; f.p.rss = mb << 20; f.p.state = st;
        f.p.basePrio = 8; f.coreUse = use; f.p.runnableThreads = st == 'R' ? 1 : 0;
        fp_[pid] = f; }
    std::map<uint32_t, FP> fp_;
    uint64_t tick_ = 0, sysTotal_ = 0, sysIdle_ = 0;
};

} // namespace
std::unique_ptr<OsApi> makeOsApi() { return std::unique_ptr<OsApi>(new FakeOs()); }
} // namespace mon
