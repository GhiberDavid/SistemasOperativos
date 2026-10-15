#include "model.h"
#include <cstdio>

namespace mon {

const char* prioName(PrioClass p) {
    switch (p) {
        case PrioClass::Idle: return "Idle (baja)";
        case PrioClass::BelowNormal: return "Inferior a normal";
        case PrioClass::Normal: return "Normal";
        case PrioClass::AboveNormal: return "Superior a normal";
        case PrioClass::High: return "Alta";
        case PrioClass::Realtime: return "Tiempo real";
    }
    return "?";
}

int prioBase(PrioClass p) {
    switch (p) {
        case PrioClass::Idle: return 4;
        case PrioClass::BelowNormal: return 6;
        case PrioClass::Normal: return 8;
        case PrioClass::AboveNormal: return 10;
        case PrioClass::High: return 13;
        case PrioClass::Realtime: return 24;
    }
    return 8;
}

const char* sortName(SortKey k) {
    switch (k) {
        case SortKey::Cpu: return "CPU";
        case SortKey::Mem: return "MEM";
        case SortKey::Pid: return "PID";
        case SortKey::Name: return "NOMBRE";
    }
    return "?";
}

CpuStats cpuStats(const std::vector<float>& h) {
    CpuStats st; st.samples = h.size();
    if (h.empty()) return st;
    double sum = 0;
    for (float v : h) { sum += v; st.max = v > st.max ? v : st.max; }
    st.avg = (float)(sum / (double)h.size());
    st.now = h.back();
    return st;
}

float niceScale(float m) { return m <= 10 ? 10.f : m <= 25 ? 25.f : m <= 50 ? 50.f : 100.f; }

std::string fmtBytes(uint64_t b) {
    char buf[32];
    double v = double(b);
    if (b < 1024ull) std::snprintf(buf, sizeof buf, "%lluB", (unsigned long long)b);
    else if (v < 1024.0 * 1024) std::snprintf(buf, sizeof buf, "%.0fK", v / 1024);
    else if (v < 1024.0 * 1024 * 1024) std::snprintf(buf, sizeof buf, v < 10.0 * 1024 * 1024 ? "%.1fM" : "%.0fM", v / (1024.0 * 1024));
    else std::snprintf(buf, sizeof buf, "%.2fG", v / (1024.0 * 1024 * 1024));
    return buf;
}

std::string fmtCpuTime(uint64_t t) {
    uint64_t cs = t / 100000ull;           // centesimas de segundo
    uint64_t s = cs / 100, c = cs % 100;
    char buf[40];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)((s / 60) % 60), (unsigned long long)(s % 60));
    else std::snprintf(buf, sizeof buf, "%llu:%02llu.%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60), (unsigned long long)c);
    return buf;
}

std::string fmtUptime(uint64_t ms) {
    uint64_t s = ms / 1000, d = s / 86400;
    char buf[40];
    if (d) std::snprintf(buf, sizeof buf, "%llud %02llu:%02llu:%02llu", (unsigned long long)d, (unsigned long long)((s / 3600) % 24), (unsigned long long)((s / 60) % 60), (unsigned long long)(s % 60));
    else std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)((s / 60) % 60), (unsigned long long)(s % 60));
    return buf;
}

} // namespace mon
