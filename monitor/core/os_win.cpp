// Implementacion para Windows. Fuentes de datos (todas APIs nativas, ningun programa externo):
//   - Tool Help (CreateToolhelp32Snapshot): lista de procesos, PID, padre, hilos, prioridad base.
//   - PSAPI / kernel32: GetProcessTimes, GetProcessMemoryInfo, GetSystemTimes, GlobalMemoryStatusEx.
//   - NtQuerySystemInformation(SystemProcessInformation): estado de cada hilo (Tool Help no lo da)
//     y respaldo de contadores para procesos protegidos.
#ifdef _WIN32
#include "os_api.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstring>

namespace mon {
namespace {

std::string toUtf8(const wchar_t* w, int len = -1) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    if (len == -1 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

uint64_t ft64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// ---------- Estructuras de ntdll (no estan completas en los headers publicos) ----------
struct NT_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct NT_THREAD_INFO {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    HANDLE UniqueProcess, UniqueThread;
    LONG Priority, BasePriority;
    ULONG ContextSwitches, ThreadState, WaitReason;
};
struct NT_PROCESS_INFO {
    ULONG NextEntryOffset, NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount, NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    NT_UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId, InheritedFromUniqueProcessId;
    ULONG HandleCount, SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount;
    LARGE_INTEGER ReadTransferCount, WriteTransferCount, OtherTransferCount;
};
struct NT_PBI { LONG ExitStatus; PVOID PebBaseAddress; ULONG_PTR AffinityMask; LONG BasePriority;
                ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId; };

typedef LONG (NTAPI *NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *NtQueryInformationProcess_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *NtProcessCtl_t)(HANDLE);

const char* kWaitReason[] = {
    "Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended", "UserRequest",
    "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution", "WrSuspended",
    "WrUserRequest", "WrEventPair", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory",
    "WrPageOut", "WrRendezvous", "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl",
    "WrCalloutStack", "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt",
    "WrPreempted", "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown"};

struct ThreadClass { char code; std::string state, wait; };

ThreadClass classify(ULONG st, ULONG wr) {
    static const char* names[] = {"Initialized", "Ready", "Running", "Standby", "Terminated", "Waiting",
                                  "Transition", "DeferredReady", "GateWait", "WaitingForProcessInUse"};
    ThreadClass c;
    c.state = st < 10 ? names[st] : "Unknown";
    c.wait = (st == 5 && wr < sizeof(kWaitReason) / sizeof(kWaitReason[0])) ? kWaitReason[wr] : "";
    switch (st) {
        case 1: case 2: case 3: case 6: case 7: c.code = 'R'; break;      // listo / corriendo
        case 4: c.code = 'Z'; break;                                      // terminado
        case 5:
            if (wr == 5 || wr == 12) c.code = 'T';                        // suspendido
            else if (wr == 2 || wr == 9) c.code = 'D';                    // esperando E/S de paginacion
            else c.code = 'S';
            break;
        default: c.code = 'S';
    }
    return c;
}

std::string winErr(DWORD e, uint32_t pid) {
    std::string p = "PID " + std::to_string(pid);
    if (e == ERROR_ACCESS_DENIED) return "Acceso denegado sobre " + p + " (proceso protegido o de otro usuario; prueba como Administrador)";
    if (e == ERROR_INVALID_PARAMETER) return "El proceso " + p + " ya no existe";
    return "Error de Windows " + std::to_string(e) + " sobre " + p;
}

struct Cached { std::string user, cmd; bool denied = false; };

class WinOs : public OsApi {
public:
    WinOs() {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) {
            ntQSI_ = (NtQuerySystemInformation_t)(void*)GetProcAddress(nt, "NtQuerySystemInformation");
            ntQIP_ = (NtQueryInformationProcess_t)(void*)GetProcAddress(nt, "NtQueryInformationProcess");
            ntSuspend_ = (NtProcessCtl_t)(void*)GetProcAddress(nt, "NtSuspendProcess");
            ntResume_ = (NtProcessCtl_t)(void*)GetProcAddress(nt, "NtResumeProcess");
        }
    }

    bool snapshot(std::vector<RawProc>& procs, RawSystem& sys, std::string& err) override {
        procs.clear();
        // --- contadores del sistema ---
        FILETIME fi, fk, fu;
        if (!GetSystemTimes(&fi, &fk, &fu)) { err = "GetSystemTimes fallo: " + std::to_string(GetLastError()); return false; }
        sys.idle = ft64(fi); sys.kernel = ft64(fk); sys.user = ft64(fu);
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) { sys.memTotal = ms.ullTotalPhys; sys.memAvail = ms.ullAvailPhys; }
        sys.uptimeMs = GetTickCount64();

        // --- estado de hilos y respaldo de contadores (NtQuerySystemInformation) ---
        struct NtProc { uint64_t cpu = 0, create = 0, ws = 0; char state = 'S'; uint32_t runnable = 0; uint32_t nthreads = 0; };
        std::map<uint32_t, NtProc> nt;
        std::vector<uint8_t> buf;
        bool haveNt = readNt(buf);
        if (haveNt) {
            forEachNtProc(buf, [&](const NT_PROCESS_INFO* p, const NT_THREAD_INFO* t, size_t n) {
                NtProc np;
                np.cpu = uint64_t(p->KernelTime.QuadPart) + uint64_t(p->UserTime.QuadPart);
                np.create = uint64_t(p->CreateTime.QuadPart);
                np.ws = p->WorkingSetSize;
                np.nthreads = p->NumberOfThreads;
                bool anyD = false, anyR = false, allSusp = n > 0;
                for (size_t i = 0; i < n; ++i) {
                    ThreadClass c = classify(t[i].ThreadState, t[i].WaitReason);
                    if (c.code == 'R') { anyR = true; np.runnable++; }
                    if (c.code == 'D') anyD = true;
                    if (c.code != 'T') allSusp = false;
                }
                np.state = np.nthreads == 0 ? 'Z' : n == 0 ? 'S' : allSusp ? 'T' : anyR ? 'R' : anyD ? 'D' : 'S';   // n == 0 con hilos: no se pudo leer el detalle
                nt[uint32_t((ULONG_PTR)p->UniqueProcessId)] = np;
            });
        }

        // --- lista de procesos (Tool Help) ---
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) { err = "CreateToolhelp32Snapshot fallo: " + std::to_string(GetLastError()); return false; }
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        std::set<std::pair<uint32_t, uint64_t>> seen;
        if (Process32FirstW(snap, &pe)) {
            do {
                RawProc r;
                r.pid = pe.th32ProcessID; r.ppid = pe.th32ParentProcessID;
                r.name = toUtf8(pe.szExeFile);
                r.threads = pe.cntThreads;
                r.basePrio = pe.pcPriClassBase;
                auto it = nt.find(r.pid);
                if (it != nt.end()) {
                    r.state = it->second.state;
                    r.runnableThreads = it->second.runnable;
                }
                if (r.pid == 0) r.name = "System Idle Process";
                HANDLE h = (r.pid == 0) ? nullptr : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid);
                bool gotTimes = false, gotMem = false;
                if (h) {
                    FILETIME c, e, k, u;
                    if (GetProcessTimes(h, &c, &e, &k, &u)) {
                        r.creation = ft64(c); r.cpu100ns = ft64(k) + ft64(u); gotTimes = true;
                    }
                    PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof(pmc);
                    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) { r.rss = pmc.WorkingSetSize; gotMem = true; }
                    DWORD code;
                    if (r.threads == 0 || (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE)) r.state = 'Z';
                }
                if (it != nt.end()) {            // respaldo para procesos que no se pudieron abrir
                    if (!gotTimes) { r.cpu100ns = it->second.cpu; r.creation = it->second.create; }
                    if (!gotMem) r.rss = it->second.ws;
                }
                if (r.threads == 0) r.state = 'Z';
                auto key = std::make_pair(r.pid, r.creation);
                seen.insert(key);
                auto ci = cache_.find(key);
                if (ci == cache_.end()) {
                    Cached c;
                    fillIdentity(h, r.pid, r.name, c);
                    ci = cache_.emplace(key, c).first;
                }
                r.user = ci->second.user; r.cmd = ci->second.cmd; r.denied = ci->second.denied || (h == nullptr && r.pid != 0);
                if (h) CloseHandle(h);
                procs.push_back(std::move(r));
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        for (auto it = cache_.begin(); it != cache_.end();) it = seen.count(it->first) ? std::next(it) : cache_.erase(it);
        return true;
    }

    bool threads(uint32_t pid, std::vector<RawThread>& out, std::string& err) override {
        out.clear();
        std::vector<uint8_t> buf;
        if (readNt(buf)) {
            bool found = false;
            forEachNtProc(buf, [&](const NT_PROCESS_INFO* p, const NT_THREAD_INFO* t, size_t n) {
                if (uint32_t((ULONG_PTR)p->UniqueProcessId) != pid) return;
                found = true;
                for (size_t i = 0; i < n; ++i) {
                    RawThread rt; ThreadClass c = classify(t[i].ThreadState, t[i].WaitReason);
                    rt.tid = uint32_t((ULONG_PTR)t[i].UniqueThread);
                    rt.basePrio = t[i].BasePriority; rt.prio = t[i].Priority;
                    rt.code = c.code; rt.state = c.state; rt.wait = c.wait;
                    rt.cpu100ns = uint64_t(t[i].KernelTime.QuadPart) + uint64_t(t[i].UserTime.QuadPart);
                    out.push_back(rt);
                }
            });
            if (!found) { err = "El proceso PID " + std::to_string(pid) + " ya no existe"; return false; }
            if (out.empty()) err = "El proceso no tiene hilos (terminado sin recoger)";
            return true;
        }
        // respaldo: Tool Help da la lista de hilos pero no su estado
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) { err = winErr(GetLastError(), pid); return false; }
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID != pid) continue;
            RawThread rt; rt.tid = te.th32ThreadID; rt.basePrio = te.tpBasePri; rt.prio = te.tpBasePri;
            rt.code = '?'; rt.state = "Desconocido";
            out.push_back(rt);
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
        if (out.empty()) { err = "El proceso PID " + std::to_string(pid) + " ya no existe"; return false; }
        return true;
    }

    ActionResult terminate(uint32_t pid, bool force) override {
        ActionResult r;
        if (pid == 0 || pid == 4) { r.message = "PID " + std::to_string(pid) + " es el sistema; no se puede terminar"; return r; }
        if (pid == GetCurrentProcessId()) { r.message = "Ese es el propio monitor; usa la tecla de salir"; return r; }
        std::string how;
        int nWin = 0;
        if (!force) {                      // 1) peticion educada: WM_CLOSE a las ventanas visibles del proceso
            struct Ctx { DWORD pid; int n; } ctx{pid, 0};
            EnumWindows([](HWND w, LPARAM lp) -> BOOL {
                Ctx* c = (Ctx*)lp; DWORD wp = 0; GetWindowThreadProcessId(w, &wp);
                if (wp == c->pid && IsWindowVisible(w)) { PostMessageW(w, WM_CLOSE, 0, 0); c->n++; }
                return TRUE; }, (LPARAM)&ctx);
            nWin = ctx.n;
        }
        DWORD access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION;
        if (force) access |= PROCESS_TERMINATE;
        else if (nWin == 0) access |= PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
        HANDLE h = OpenProcess(access, FALSE, pid);
        if (!h) {
            DWORD e = GetLastError();
            if (nWin > 0 && e == ERROR_ACCESS_DENIED) { r.ok = true; r.message = "se pidio cerrar " + std::to_string(nWin) + " ventana(s) (WM_CLOSE); no se pudo comprobar si termino"; return r; }
            r.message = winErr(e, pid); return r;
        }
        if (force) {
            if (!TerminateProcess(h, 1)) { r.message = winErr(GetLastError(), pid); CloseHandle(h); return r; }
            how = "terminado por la fuerza (TerminateProcess)";
        } else if (nWin > 0) {
            how = "se pidio cerrar " + std::to_string(nWin) + " ventana(s) (WM_CLOSE)";
        } else {
            // 2) sin ventanas: hilo remoto que llama a ExitProcess (salida ordenada)
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            LPTHREAD_START_ROUTINE ep = (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(k32, "ExitProcess");
            HANDLE t = ep ? CreateRemoteThread(h, nullptr, 0, ep, (LPVOID)(ULONG_PTR)0, 0, nullptr) : nullptr;
            if (!t) {
                r.message = "no tiene ventanas ni acepta salida ordenada; usa terminar por la fuerza (K)";
                CloseHandle(h); return r;
            }
            CloseHandle(t);
            how = "terminado con salida ordenada (ExitProcess)";
        }
        DWORD w = WaitForSingleObject(h, 2000);
        CloseHandle(h);
        if (w == WAIT_OBJECT_0) { r.ok = true; r.message = how + "; el proceso ya no esta"; }
        else { r.ok = true; r.message = how + "; sigue activo (puede estar cerrando o pidiendo confirmacion)"; }
        return r;
    }

    ActionResult suspend(uint32_t pid) override { return suspendResume(pid, true); }
    ActionResult resume(uint32_t pid) override { return suspendResume(pid, false); }

    ActionResult getPriority(uint32_t pid, PrioClass& cls) override {
        ActionResult r;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) { r.message = winErr(GetLastError(), pid); return r; }
        DWORD c = GetPriorityClass(h); DWORD e = GetLastError(); CloseHandle(h);
        if (!c) { r.message = winErr(e, pid); return r; }
        switch (c) {
            case IDLE_PRIORITY_CLASS: cls = PrioClass::Idle; break;
            case BELOW_NORMAL_PRIORITY_CLASS: cls = PrioClass::BelowNormal; break;
            case ABOVE_NORMAL_PRIORITY_CLASS: cls = PrioClass::AboveNormal; break;
            case HIGH_PRIORITY_CLASS: cls = PrioClass::High; break;
            case REALTIME_PRIORITY_CLASS: cls = PrioClass::Realtime; break;
            default: cls = PrioClass::Normal;
        }
        r.ok = true; r.message = std::string("prioridad actual: ") + prioName(cls);
        return r;
    }

    ActionResult setPriority(uint32_t pid, PrioClass cls) override {
        ActionResult r;
        if (pid == 0 || pid == 4) { r.message = "PID " + std::to_string(pid) + " es el sistema; no se puede cambiar su prioridad"; return r; }
        DWORD c = NORMAL_PRIORITY_CLASS;
        switch (cls) {
            case PrioClass::Idle: c = IDLE_PRIORITY_CLASS; break;
            case PrioClass::BelowNormal: c = BELOW_NORMAL_PRIORITY_CLASS; break;
            case PrioClass::Normal: c = NORMAL_PRIORITY_CLASS; break;
            case PrioClass::AboveNormal: c = ABOVE_NORMAL_PRIORITY_CLASS; break;
            case PrioClass::High: c = HIGH_PRIORITY_CLASS; break;
            case PrioClass::Realtime: c = REALTIME_PRIORITY_CLASS; break;
        }
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) { r.message = winErr(GetLastError(), pid); return r; }
        BOOL ok = SetPriorityClass(h, c); DWORD e = GetLastError(); CloseHandle(h);
        if (!ok) { r.message = winErr(e, pid); return r; }
        r.ok = true; r.message = std::string("prioridad cambiada a ") + prioName(cls);
        return r;
    }

private:
    NtQuerySystemInformation_t ntQSI_ = nullptr;
    NtQueryInformationProcess_t ntQIP_ = nullptr;
    NtProcessCtl_t ntSuspend_ = nullptr, ntResume_ = nullptr;
    std::map<std::pair<uint32_t, uint64_t>, Cached> cache_;

    bool readNt(std::vector<uint8_t>& buf) {
        if (!ntQSI_) return false;
        ULONG need = buf.empty() ? (1u << 20) : (ULONG)buf.size();
        for (int tries = 0; tries < 8; ++tries) {
            buf.assign(need, 0);
            ULONG got = 0;
            LONG st = ntQSI_(5 /*SystemProcessInformation*/, buf.data(), (ULONG)buf.size(), &got);
            if (st >= 0) return true;
            if (st == (LONG)0xC0000004L) need = (got ? got : need) + (256u << 10); // STATUS_INFO_LENGTH_MISMATCH
            else return false;
        }
        return false;
    }

    template <class F> void forEachNtProc(const std::vector<uint8_t>& buf, F f) {
        size_t off = 0;
        while (off + sizeof(NT_PROCESS_INFO) <= buf.size()) {
            const NT_PROCESS_INFO* p = reinterpret_cast<const NT_PROCESS_INFO*>(buf.data() + off);
            const NT_THREAD_INFO* t = reinterpret_cast<const NT_THREAD_INFO*>(buf.data() + off + sizeof(NT_PROCESS_INFO));
            size_t n = p->NumberOfThreads;
            size_t end = off + sizeof(NT_PROCESS_INFO) + n * sizeof(NT_THREAD_INFO);
            if (end > buf.size()) n = 0;                       // estructura inesperada: no leer fuera del buffer
            else if (n > 0 && (ULONG_PTR)t[0].UniqueProcess != (ULONG_PTR)p->UniqueProcessId) n = 0; // diseno distinto al esperado
            f(p, t, n);
            if (p->NextEntryOffset == 0) break;
            off += p->NextEntryOffset;
        }
    }

    void fillIdentity(HANDLE h, uint32_t pid, const std::string& name, Cached& c) {
        c.cmd = name; c.user = "?";
        if (pid == 0 || pid == 4) { c.user = "SYSTEM"; return; }
        if (!h) { c.denied = true; return; }
        wchar_t path[MAX_PATH * 2]; DWORD n = (DWORD)(sizeof(path) / sizeof(path[0]));
        if (QueryFullProcessImageNameW(h, 0, path, &n)) c.cmd = toUtf8(path, (int)n);
        HANDLE tok = nullptr;
        if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
            DWORD sz = 0; GetTokenInformation(tok, TokenUser, nullptr, 0, &sz);
            std::vector<uint8_t> tb(sz ? sz : 1);
            if (sz && GetTokenInformation(tok, TokenUser, tb.data(), sz, &sz)) {
                wchar_t nm[256], dom[256]; DWORD nl = 256, dl = 256; SID_NAME_USE use;
                if (LookupAccountSidW(nullptr, ((TOKEN_USER*)tb.data())->User.Sid, nm, &nl, dom, &dl, &use)) c.user = toUtf8(nm);
            }
            CloseHandle(tok);
        }
#ifdef _WIN64
        // linea de comando real leyendo el PEB del proceso (necesita mas permisos; si falla se queda la ruta)
        if (ntQIP_) {
            HANDLE hr = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hr) {
                NT_PBI pbi; memset(&pbi, 0, sizeof(pbi)); ULONG rl = 0;
                if (ntQIP_(hr, 0, &pbi, sizeof(pbi), &rl) >= 0 && pbi.PebBaseAddress) {
                    PVOID params = nullptr; SIZE_T rd = 0;
                    if (ReadProcessMemory(hr, (uint8_t*)pbi.PebBaseAddress + 0x20, &params, sizeof(params), &rd) && params) {
                        NT_UNICODE_STRING us; memset(&us, 0, sizeof(us));
                        if (ReadProcessMemory(hr, (uint8_t*)params + 0x70, &us, sizeof(us), &rd) && us.Buffer && us.Length > 0 && us.Length < 8192) {
                            std::vector<wchar_t> w(us.Length / 2 + 1, 0);
                            if (ReadProcessMemory(hr, us.Buffer, w.data(), us.Length, &rd)) {
                                std::string s = toUtf8(w.data());
                                if (!s.empty()) c.cmd = s;
                            }
                        }
                    }
                }
                CloseHandle(hr);
            }
        }
#endif
    }

    ActionResult suspendResume(uint32_t pid, bool suspend) {
        ActionResult r;
        if (pid == 0 || pid == 4) { r.message = "PID " + std::to_string(pid) + " es el sistema; no se puede suspender"; return r; }
        if (pid == GetCurrentProcessId()) { r.message = "No se puede suspender el propio monitor"; return r; }
        NtProcessCtl_t fn = suspend ? ntSuspend_ : ntResume_;
        if (!fn) { r.message = "Esta version de Windows no expone NtSuspendProcess/NtResumeProcess"; return r; }
        HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) { r.message = winErr(GetLastError(), pid); return r; }
        LONG st = fn(h); CloseHandle(h);
        if (st < 0) { r.message = "el sistema rechazo la operacion (codigo " + std::to_string((unsigned long)st) + ")"; return r; }
        r.ok = true;
        r.message = suspend ? "suspendido (todos sus hilos detenidos)" : "reanudado, continua su ejecucion";
        return r;
    }
};

} // namespace

std::unique_ptr<OsApi> makeOsApi() { return std::unique_ptr<OsApi>(new WinOs()); }

} // namespace mon
#endif // _WIN32
