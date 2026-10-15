#include "../core/session.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace mon;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FALLO linea %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static const ProcRow* get(const Snapshot& s, uint32_t pid) { for (auto& r : s.rows) if (r.pid == pid) return &r; return nullptr; }

int main() {
    Monitor m(makeOsApi());
    CHECK(m.refresh());
    auto s0 = m.snapshot();
    CHECK(s0.sum.total == 11);                      // sin el proceso Idle
    CHECK(get(s0, 310) && get(s0, 310)->cpu == 0);  // primera muestra: sin diferencia todavia
    CHECK(s0.sum.zombie == 1 && s0.sum.blocked == 1 && s0.sum.running == 2);
    std::string e;
    for (int i = 0; i < 3; ++i) { CHECK(m.refresh(&e)); std::this_thread::yield(); }
    // refresco inmediato (< 50 ms de tiempo real no importa: el tiempo lo da el "SO" simulado)
    auto s = m.snapshot();
    // chrome (310) usa 1 nucleo de 4 => 25 % de la maquina; 311 usa medio nucleo => 12.5 %
    CHECK(std::fabs(get(s, 310)->cpu - 25.0) < 0.01);
    CHECK(std::fabs(get(s, 311)->cpu - 12.5) < 0.01);
    CHECK(std::fabs(get(s, 320)->cpu - 6.25) < 0.01);
    CHECK(std::fabs(s.sum.cpuTotal - (1 + .5 + .25 + .1 + .02 + .01) / 4 * 100) < 0.01);
    CHECK(s.sum.memTotal == (16ull << 30) && s.sum.memUsed == (6ull << 30) && s.sum.memAvail == (10ull << 30));
    // orden por CPU: el que mas consume primero
    CHECK(s.rows.front().pid == 310);
    for (size_t i = 1; i < s.rows.size(); ++i) CHECK(s.rows[i - 1].cpu >= s.rows[i].cpu);
    m.setSort(SortKey::Mem); s = m.snapshot();
    CHECK(s.rows.front().pid == 310);               // 710 MB
    for (size_t i = 1; i < s.rows.size(); ++i) CHECK(s.rows[i - 1].rss >= s.rows[i].rss);
    m.setSort(SortKey::Pid); s = m.snapshot(); CHECK(s.rows.front().pid == 4);
    m.setSort(SortKey::Name); s = m.snapshot(); CHECK(s.rows.front().name == "chrome.exe");
    // filtro
    m.setFilter("CHROME"); s = m.snapshot(); CHECK(s.rows.size() == 3 && s.hiddenByFilter == 8);
    m.setFilter("ghiber"); s = m.snapshot(); CHECK(s.rows.size() == 7);
    m.setFilter("");
    // arbol
    m.setTree(true); m.setSort(SortKey::Cpu); s = m.snapshot();
    CHECK(s.rows.size() == 11);
    CHECK(s.rows[0].pid == 4 && s.rows[0].depth == 0);
    int idx310 = -1, idx300 = -1, idx311 = -1;
    for (size_t i = 0; i < s.rows.size(); ++i) { if (s.rows[i].pid == 310) idx310 = (int)i; if (s.rows[i].pid == 300) idx300 = (int)i; if (s.rows[i].pid == 311) idx311 = (int)i; }
    CHECK(idx300 < idx310 && idx310 < idx311);      // padre antes que hijo, hijos justo despues
    CHECK(s.rows[idx311].depth == 3 && s.rows[idx310].depth == 2);
    CHECK(!s.rows[idx311].treePrefix.empty());
    m.setFilter("311"); s = m.snapshot();           // el filtro en arbol conserva los ancestros
    CHECK(s.rows.size() == 4 && s.rows.back().pid == 311);
    m.setFilter(""); m.setTree(false);
    // historico
    auto h = m.cpuHistory(310); CHECK(h.size() >= 3 && std::fabs(h.back() - 25.0) < 0.01);
    CHECK(s.sum.cpuHistory.size() >= 3);
    // hilos
    std::vector<RawThread> th; CHECK(m.threads(310, th, e) && th.size() == 45);
    CHECK(!m.threads(99999, th, e) && e.find("ya no existe") != std::string::npos);
    // acciones
    auto r = m.suspend(310); CHECK(r.ok); s = m.snapshot(); CHECK(get(s, 310)->state == 'T' && s.sum.stopped == 1);
    r = m.resume(310); CHECK(r.ok); s = m.snapshot(); CHECK(get(s, 310)->state == 'R' && s.sum.stopped == 0);
    r = m.terminate(312, false); CHECK(r.ok); s = m.snapshot(); CHECK(get(s, 312) == nullptr && s.sum.total == 10);
    r = m.terminate(312, true); CHECK(!r.ok && r.message.find("ya no existe") != std::string::npos);
    r = m.terminate(350, true); CHECK(!r.ok && r.message.find("denegado") != std::string::npos);
    PrioClass pc; r = m.getPriority(320, pc); CHECK(r.ok && pc == PrioClass::Normal);
    r = m.setPriority(320, PrioClass::High); CHECK(r.ok); m.getPriority(320, pc); CHECK(pc == PrioClass::High);
    s = m.snapshot(); CHECK(get(s, 320)->prio == 13);
    // intervalo
    m.setIntervalMs(10); CHECK(m.intervalMs() == 250); m.setIntervalMs(99999); CHECK(m.intervalMs() == 10000); m.setIntervalMs(1000);
    // formato
    { auto c = cpuStats({10, 20, 30}); CHECK(c.now == 30 && c.max == 30 && std::fabs(c.avg - 20) < 0.01 && c.samples == 3); }
    CHECK(cpuStats({}).samples == 0 && niceScale(7) == 10 && niceScale(30) == 50 && niceScale(80) == 100);
    CHECK(fmtBytes(710ull << 20) == "710M"); CHECK(fmtCpuTime(72ull * 10000000) == "1:12.00");
    // ---- Session: la logica de teclas compartida por TUI y GUI ----
    {
        Monitor m2(makeOsApi()); m2.refresh(); m2.refresh(); m2.refresh();
        Session se(m2); se.onRefresh();
        auto ch = [&](const char* c) { return se.handle(Key::chr(c), 10); };
        CHECK(se.current() && se.current()->pid == 310);        // arranca sobre el que mas consume
        se.handle(Key::of(KeyType::Down), 10); CHECK(se.current()->pid == 311);
        ch("h"); CHECK(se.view == ViewMode::Threads && se.threads.size() == 20 && se.thPid == 311);
        se.handle(Key::of(KeyType::Escape), 10); CHECK(se.view == ViewMode::Table);
        ch("k"); CHECK(se.input == InputMode::Confirm && se.pendPid == 311);
        ch("n"); CHECK(se.input == InputMode::None && se.message().find("Cancelado") == 0 && m2.find(311));
        ch("k"); ch("s"); CHECK(!m2.find(311) && se.message().find("OK Terminar") == 0 && se.message().find("311") != std::string::npos);
        se.onRefresh(); CHECK(se.snap.sum.total == 10);
        ch("/"); CHECK(se.input == InputMode::Filter); ch("c"); ch("o"); ch("d"); CHECK(se.snap.rows.size() == 1);
        se.handle(Key::of(KeyType::Escape), 10); CHECK(se.snap.rows.size() == 10);   // Esc cancela el filtro
        ch("f"); ch("7"); ch("0"); ch("0"); se.handle(Key::of(KeyType::Enter), 10); CHECK(m2.intervalMs() == 700);
        ch("t"); CHECK(m2.tree() && se.snap.rows[0].depth == 0); ch("t"); CHECK(!m2.tree());
        ch("m"); CHECK(m2.sort() == SortKey::Mem); ch("c"); CHECK(m2.sort() == SortKey::Cpu);
        CHECK(ch("?") == KeyResult::Handled && se.input == InputMode::Help); ch("x"); CHECK(se.input == InputMode::None);
        // historico (tecla g): abre la vista, las flechas cambian de proceso, g o Esc vuelven a la tabla
        ch("g"); CHECK(se.view == ViewMode::History && se.current());
        { uint32_t a = se.current()->pid; se.handle(Key::of(KeyType::Down), 10); CHECK(se.view == ViewMode::History && se.current()->pid != a); }
        CHECK(!m2.cpuHistory(se.current()->pid).empty());
        se.handle(Key::of(KeyType::Escape), 10); CHECK(se.view == ViewMode::Table);
        ch("g"); ch("g"); CHECK(se.view == ViewMode::Table);
        CHECK(ch("q") == KeyResult::Quit);
        // proceso sin permiso: mensaje claro y no se cae
        m2.setFilter("protegido"); se.reload(); ch("K"); CHECK(se.input == InputMode::Confirm); ch("s");
        CHECK(se.message().find("ERROR") == 0 && se.message().find("denegado") != std::string::npos);
        ch("p"); CHECK(se.input == InputMode::None && se.message().find("ERROR") == 0);
        m2.setFilter(""); se.reload();
    }
    std::printf(fails ? "\n%d fallos\n" : "TODAS LAS PRUEBAS DEL CORE PASARON\n", fails);
    return fails ? 1 : 0;
}
