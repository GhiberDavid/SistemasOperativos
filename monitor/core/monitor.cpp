#include "monitor.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <unordered_map>

namespace mon {

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

Monitor::Monitor(std::unique_ptr<OsApi> os) : os_(std::move(os)) {}

bool Monitor::refresh(std::string* err) {
    std::lock_guard<std::mutex> rl(refreshMu_);
    std::vector<RawProc> raw; RawSystem rs; std::string e;
    bool ok = os_->snapshot(raw, rs, e);   // llamada lenta al SO: fuera del candado
    if (!ok) { if (err) *err = e; return false; }
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(mu_);
    uint64_t sysTotal = rs.kernel + rs.user;           // Total del sistema , kernel ya incluye el tiempo idle
    uint64_t dSys = havePrev_ ? sysTotal - prevSysTotal_ : 0; // Tiempo de CPU que paso en toda la maquina
    uint64_t dIdle = havePrev_ ? rs.idle - prevSysIdle_ : 0;
    // Con muy poco tiempo entre muestras (por ejemplo refresco tras una accion) el cociente es ruidoso:
    // se conservan los porcentajes anteriores y no se actualiza la base de comparacion.
    bool enough = havePrev_ && dSys >= 500000;         // >= 50 ms de tiempo total de CPU
    double dtSec = havePrev_ ? std::chrono::duration<double>(now - prevTime_).count() : 0;

    std::vector<Internal> next; next.reserve(raw.size());
    std::map<Key, uint64_t> newPrev;
    std::map<Key, double> newPct;
    std::map<Key, std::deque<float>> newHist;
    Summary s;
    uint64_t runnable = 0;
    for (auto& r : raw) {
        if (r.pid == 0) continue;                      // "System Idle Process" no es un proceso real
        Key k{r.pid, r.creation};
        double pct = 0;
        auto pi = prevCpu_.find(k);
        if (enough) {
            if (pi != prevCpu_.end() && r.cpu100ns >= pi->second)
                pct = 100.0 * double(r.cpu100ns - pi->second) / double(dSys);   // dif. proceso / dif. sistema
            newPrev[k] = r.cpu100ns;
        } else {
            auto lp = lastPct_.find(k);
            if (lp != lastPct_.end()) pct = lp->second;
            newPrev[k] = (pi != prevCpu_.end()) ? pi->second : r.cpu100ns;
        }
        pct = std::min(100.0, std::max(0.0, pct));
        newPct[k] = pct;

        Internal in; in.creation = r.creation;
        ProcRow& w = in.row;
        w.pid = r.pid; w.ppid = r.ppid; w.user = r.user; w.name = r.name; w.cmd = r.cmd;
        w.state = r.state; w.cpu = pct; w.rss = r.rss; w.threads = r.threads; w.prio = r.basePrio;
        w.cpuTime = r.cpu100ns; w.denied = r.denied;
        w.mem = rs.memTotal ? 100.0 * double(r.rss) / double(rs.memTotal) : 0;
        next.push_back(std::move(in));

        auto& h = newHist[k];
        auto old = hist_.find(k);
        if (old != hist_.end()) h = old->second;
        if (enough || h.empty()) { h.push_back((float)pct); if (h.size() > kHist) h.pop_front(); }

        s.total++;
        switch (r.state) {
            case 'R': s.running++; break;
            case 'T': s.stopped++; break;
            case 'Z': s.zombie++; break;
            case 'D': s.blocked++; break;
            default: s.sleeping++;
        }
        runnable += r.runnableThreads + (r.state == 'D' ? 1 : 0);
    }

    // CPU global
    double sysPct = lastSysPct_;
    if (enough) {
        sysPct = dSys ? 100.0 * (1.0 - double(dIdle) / double(dSys)) : 0;
        sysPct = std::min(100.0, std::max(0.0, sysPct));
        lastSysPct_ = sysPct;
    }
    s.cpuTotal = sysPct;
    s.cpuHistory = sum_.cpuHistory;
    if (enough || s.cpuHistory.empty()) { s.cpuHistory.push_back((float)sysPct); if (s.cpuHistory.size() > kHist) s.cpuHistory.erase(s.cpuHistory.begin()); }

    // Carga promedio: Windows no la tiene; se calcula como en Unix con medias moviles
    // exponenciales (1, 5 y 15 min) del numero de hilos listos/corriendo + procesos en espera de E/S.
    s.load1 = sum_.load1; s.load5 = sum_.load5; s.load15 = sum_.load15;
    if (havePrev_ && dtSec > 0) {
        double n = double(runnable);
        auto ema = [&](double cur, double window) { double a = std::exp(-dtSec / window); return cur * a + n * (1 - a); };
        s.load1 = ema(s.load1, 60); s.load5 = ema(s.load5, 300); s.load15 = ema(s.load15, 900);
    }

    s.memTotal = rs.memTotal; s.memAvail = rs.memAvail;
    s.memUsed = rs.memTotal > rs.memAvail ? rs.memTotal - rs.memAvail : 0;
    s.uptimeMs = rs.uptimeMs;
    s.refreshes = sum_.refreshes + 1;

    if (enough || !havePrev_) { prevSysTotal_ = sysTotal; prevSysIdle_ = rs.idle; prevTime_ = now; }
    havePrev_ = true;
    procs_ = std::move(next);
    prevCpu_ = std::move(newPrev);
    lastPct_ = std::move(newPct);
    hist_ = std::move(newHist);
    sum_ = std::move(s);
    return true;
}

Snapshot Monitor::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    Snapshot out; out.sum = sum_;

    std::string f = lower(filter_);
    std::vector<const Internal*> vis;
    std::set<uint32_t> matched;
    for (auto& p : procs_) {
        bool m = f.empty() || lower(p.row.name).find(f) != std::string::npos
                 || lower(p.row.cmd).find(f) != std::string::npos
                 || lower(p.row.user).find(f) != std::string::npos
                 || std::to_string(p.row.pid).find(f) != std::string::npos;
        if (m) matched.insert(p.row.pid);
    }
    std::unordered_map<uint32_t, const Internal*> byPid;
    for (auto& p : procs_) byPid[p.row.pid] = &p;
    std::set<uint32_t> show = matched;
    if (tree_ && !f.empty()) {       // en el arbol se conservan los ancestros para no romper la jerarquia
        for (uint32_t pid : matched) {
            uint32_t cur = pid;
            for (int guard = 0; guard < 64; ++guard) {
                auto it = byPid.find(cur);
                if (it == byPid.end()) break;
                uint32_t par = it->second->row.ppid;
                auto pit = byPid.find(par);
                if (par == cur || pit == byPid.end() || pit->second->creation > it->second->creation) break;
                show.insert(par); cur = par;
            }
        }
    }
    for (auto& p : procs_) if (show.count(p.row.pid)) vis.push_back(&p);
    out.hiddenByFilter = (int)procs_.size() - (int)vis.size();

    auto less = [&](const Internal* a, const Internal* b) {
        const ProcRow &x = a->row, &y = b->row;
        switch (sort_) {
            case SortKey::Cpu: if (x.cpu != y.cpu) return x.cpu > y.cpu; break;
            case SortKey::Mem: if (x.rss != y.rss) return x.rss > y.rss; break;
            case SortKey::Name: { std::string nx = lower(x.name), ny = lower(y.name); if (nx != ny) return nx < ny; break; }
            case SortKey::Pid: break;
        }
        return x.pid < y.pid;
    };
    std::sort(vis.begin(), vis.end(), less);

    if (!tree_) {
        for (auto* p : vis) out.rows.push_back(p->row);
        return out;
    }

    // ----- vista de arbol padre -> hijo -----
    std::unordered_map<uint32_t, const Internal*> inSet;
    for (auto* p : vis) inSet[p->row.pid] = p;
    std::unordered_map<uint32_t, std::vector<const Internal*>> kids;
    std::vector<const Internal*> roots;
    for (auto* p : vis) {
        auto pit = inSet.find(p->row.ppid);
        bool hasParent = pit != inSet.end() && p->row.ppid != p->row.pid && pit->second->creation <= p->creation;
        if (hasParent) kids[p->row.ppid].push_back(p); else roots.push_back(p);
    }
    std::set<uint32_t> visited;
    std::function<void(const Internal*, int, const std::string&, bool)> walk =
        [&](const Internal* p, int depth, const std::string& indent, bool last) {
            if (!visited.insert(p->row.pid).second) return;
            ProcRow r = p->row; r.depth = depth;
            r.treePrefix = depth == 0 ? "" : indent + (last ? "└─ " : "├─ ");
            out.rows.push_back(r);
            auto it = kids.find(p->row.pid);
            if (it == kids.end()) return;
            std::string childIndent = depth == 0 ? "" : indent + (last ? "   " : "│  ");
            for (size_t i = 0; i < it->second.size(); ++i)
                walk(it->second[i], depth + 1, childIndent, i + 1 == it->second.size());
        };
    for (size_t i = 0; i < roots.size(); ++i) walk(roots[i], 0, "", i + 1 == roots.size());
    return out;
}

void Monitor::setSort(SortKey k) { std::lock_guard<std::mutex> lk(mu_); sort_ = k; }
SortKey Monitor::sort() const { std::lock_guard<std::mutex> lk(mu_); return sort_; }
void Monitor::setFilter(const std::string& f) { std::lock_guard<std::mutex> lk(mu_); filter_ = f; }
std::string Monitor::filter() const { std::lock_guard<std::mutex> lk(mu_); return filter_; }
void Monitor::setTree(bool on) { std::lock_guard<std::mutex> lk(mu_); tree_ = on; }
bool Monitor::tree() const { std::lock_guard<std::mutex> lk(mu_); return tree_; }
void Monitor::setIntervalMs(int ms) { std::lock_guard<std::mutex> lk(mu_); intervalMs_ = std::min(10000, std::max(250, ms)); }
int Monitor::intervalMs() const { std::lock_guard<std::mutex> lk(mu_); return intervalMs_; }

std::optional<ProcRow> Monitor::find(uint32_t pid) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : procs_) if (p.row.pid == pid) return p.row;
    return std::nullopt;
}

std::vector<float> Monitor::cpuHistory(uint32_t pid) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : procs_) if (p.row.pid == pid) {
        auto it = hist_.find(Key{p.row.pid, p.creation});
        if (it != hist_.end()) return std::vector<float>(it->second.begin(), it->second.end());
    }
    return {};
}

bool Monitor::threads(uint32_t pid, std::vector<RawThread>& out, std::string& err) {
    err.clear();
    return os_->threads(pid, out, err);
}

ActionResult Monitor::terminate(uint32_t pid, bool force) { auto r = os_->terminate(pid, force); refresh(); return r; }
ActionResult Monitor::suspend(uint32_t pid) { auto r = os_->suspend(pid); refresh(); return r; }
ActionResult Monitor::resume(uint32_t pid) { auto r = os_->resume(pid); refresh(); return r; }
ActionResult Monitor::getPriority(uint32_t pid, PrioClass& cls) { return os_->getPriority(pid, cls); }
ActionResult Monitor::setPriority(uint32_t pid, PrioClass cls) { auto r = os_->setPriority(pid, cls); refresh(); return r; }

} // namespace mon
