// Interfaz TUI (terminal a pantalla completa) con FTXUI. Toda la logica esta en core/.
#include "../core/session.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace ftxui;
using namespace mon;

namespace {

std::string f(const char* fmt, double v) { char b[64]; std::snprintf(b, sizeof b, fmt, v); return b; }

Element cell(const std::string& s, int w, bool right = false) {
    Element t = right ? hbox({filler(), text(s)}) : text(s);
    return t | size(WIDTH, EQUAL, w);
}

Color cpuColor(double v) { return v >= 60 ? Color::Red : v >= 25 ? Color::Yellow : Color::Green; }

Element sparkline(std::vector<float> h, float scaleMax) {
    return graph([h, scaleMax](int w, int hgt) {
        std::vector<int> d((size_t)w, 0);
        int n = (int)h.size();
        for (int i = 0; i < w; ++i) {
            int idx = n - (w - i);
            float v = idx >= 0 ? h[(size_t)idx] : 0.f;
            d[(size_t)i] = std::min(hgt - 1, std::max(0, (int)(v / scaleMax * (hgt - 1) + 0.5f)));
        }
        return d;
    });
}

// Grafico grande del historico (vista g). El eje X son siempre las ultimas Monitor::kHist muestras:
// la mas nueva a la derecha; si aun hay pocas, el lado izquierdo queda vacio.
Element historyGraph(std::vector<float> h, float scaleMax) {
    return graph([h, scaleMax](int w, int hgt) {
        std::vector<int> d((size_t)w, 0);
        const int slots = (int)Monitor::kHist, n = (int)h.size();
        for (int i = 0; i < w; ++i) {
            int idx = n - (slots - i * slots / w);          // muestra que cae en esta columna
            if (idx < 0) { d[(size_t)i] = -1; continue; }   // todavia sin muestra: columna vacia
            d[(size_t)i] = std::min(hgt - 1, std::max(0, (int)(h[(size_t)idx] / scaleMax * (hgt - 1) + 0.5f)));
        }
        return d;
    });
}

// Eje Y: el tope arriba, la mitad al centro y 0 abajo
Element yAxis(float top) {
    return vbox({text(f("%.0f%%", top)), filler(), text(f(std::fmod(top, 2.f) ? "%.1f%%" : "%.0f%%", top / 2)), filler(), text("0%")}) | size(WIDTH, EQUAL, 5);
}

std::string agoText(double secs) {
    char b[48];
    if (secs >= 60) std::snprintf(b, sizeof b, "hace ~%.0f min", secs / 60); else std::snprintf(b, sizeof b, "hace ~%.0f s", secs);
    return b;
}

struct Ui {
    Monitor& mon;
    ScreenInteractive& screen;
    Session s;

    Ui(Monitor& m, ScreenInteractive& sc) : mon(m), screen(sc), s(m) {}

    int tableRows() const { return std::max(1, Terminal::Size().dimy - 11); }

    bool handle(Event e) {
        if (e == Event::Custom) { s.onRefresh(); return true; }
        Key k;
        if (e.is_mouse()) {
            if (e.mouse().button == Mouse::WheelUp) k = Key::of(KeyType::WheelUp);
            else if (e.mouse().button == Mouse::WheelDown) k = Key::of(KeyType::WheelDown);
            else return false;
        }
        else if (e == Event::ArrowUp) k = Key::of(KeyType::Up);
        else if (e == Event::ArrowDown) k = Key::of(KeyType::Down);
        else if (e == Event::ArrowLeft) k = Key::of(KeyType::Left);
        else if (e == Event::ArrowRight) k = Key::of(KeyType::Right);
        else if (e == Event::PageUp) k = Key::of(KeyType::PageUp);
        else if (e == Event::PageDown) k = Key::of(KeyType::PageDown);
        else if (e == Event::Home) k = Key::of(KeyType::Home);
        else if (e == Event::End) k = Key::of(KeyType::End);
        else if (e == Event::Return) k = Key::of(KeyType::Enter);
        else if (e == Event::Escape) k = Key::of(KeyType::Escape);
        else if (e == Event::Backspace) k = Key::of(KeyType::Backspace);
        else if (e.is_character()) k = Key::chr(e.character());
        else return false;
        KeyResult r = s.handle(k, tableRows());
        if (r == KeyResult::Quit) screen.Exit();
        return r != KeyResult::Ignored;
    }

    // ---------- dibujo ----------
    Element renderHeader(int width) {
        const Summary& sm = s.snap.sum;
        auto stat = [](const std::string& l, int v, Color c) {
            return hbox({text(l + " ") | dim, text(std::to_string(v)) | bold | color(c), text("  ")});
        };
        Element l1 = hbox({stat("Procesos", sm.total, Color::White), stat("Corriendo", sm.running, Color::Green),
                           stat("E/S", sm.blocked, Color::Yellow), stat("Detenidos", sm.stopped, Color::Magenta),
                           stat("Zombis", sm.zombie, Color::Red), stat("Dormidos", sm.sleeping, Color::GrayLight)});
        Element l2 = hbox({text("CPU ") | bold, gauge((float)(sm.cpuTotal / 100)) | flex | color(cpuColor(sm.cpuTotal)),
                           text(" " + f("%5.1f", sm.cpuTotal) + "%   "),
                           text("Carga ") | dim, text(f("%.2f ", sm.load1) + f("%.2f ", sm.load5) + f("%.2f", sm.load15)) | bold});
        double mp = sm.memTotal ? 100.0 * double(sm.memUsed) / double(sm.memTotal) : 0;
        Element l3 = hbox({text("MEM ") | bold, gauge((float)(mp / 100)) | flex | color(cpuColor(mp)),
                           text(" " + fmtBytes(sm.memUsed) + " / " + fmtBytes(sm.memTotal) + " usada (" + f("%.0f", mp) + "%)   disp. " + fmtBytes(sm.memAvail))});
        std::string flt = mon.filter();
        Element l4 = hbox({text("Orden ") | dim, text(sortName(mon.sort())) | bold, text("  Vista ") | dim,
                           text(s.view == ViewMode::Threads ? "hilos" : s.view == ViewMode::History ? "historico" : mon.tree() ? "arbol" : "tabla") | bold,
                           text("  Refresco ") | dim, text(std::to_string(mon.intervalMs()) + " ms") | bold,
                           text("  Filtro ") | dim, text(flt.empty() ? "-" : flt) | bold | color(Color::Cyan),
                           text("  Activo ") | dim, text(fmtUptime(sm.uptimeMs))});
        Element left = vbox({l1, l2, l3, l4}) | flex;
        if (width < 120) return window(text(" Resumen ") | bold, left);

        Element sysG = vbox({text("CPU total " + f("%.0f", sm.cpuTotal) + "% /100") | dim, sparkline(sm.cpuHistory, 100.f) | color(cpuColor(sm.cpuTotal)) | size(HEIGHT, EQUAL, 3)}) | flex;
        std::vector<float> ph; std::string ptitle = "(sin proceso)"; float pmax = 10.f;
        if (auto* p = s.current()) {
            ph = mon.cpuHistory(p->pid);
            for (float v : ph) pmax = std::max(pmax, v);
            ptitle = p->name + " " + f("%.1f", p->cpu) + "% /" + f("%.0f", pmax);
        }
        Element procG = vbox({text("Hist. " + ptitle) | dim | size(WIDTH, LESS_THAN, 22), sparkline(ph, pmax) | color(Color::Cyan) | size(HEIGHT, EQUAL, 3)}) | flex;
        Element right = hbox({sysG, text(" "), procG}) | size(WIDTH, EQUAL, 46);
        return window(text(" Resumen ") | bold, hbox({left, separator(), right}));
    }

    Element stateStyle(Element e, char st, bool denied, bool selected) {
        if (st == 'Z') e = e | color(Color::Red) | bold;
        else if (st == 'D') e = e | color(Color::Yellow);
        else if (st == 'T') e = e | color(Color::Magenta);
        else if (denied) e = e | dim;
        if (selected) e = e | inverted;
        return e;
    }

    Element renderTable(int width) {
        bool wide = width >= 112;
        auto head = hbox({cell("PID", 6, true), text(" "), cell("PPID", 6, true), text(" "), cell("USUARIO", 12), text(" "), cell("E", 2),
                          cell("%CPU", 5, true), text(" "), cell("%MEM", 5, true), text(" "), cell("RSS", 7, true), text(" "), cell("HIL", 4, true), text(" "),
                          cell("PRI", 3, true), text(" "), cell("TIEMPO CPU", 10, true), text(" "), wide ? cell("PROGRAMA", 18) : text(""),
                          wide ? text(" ") : text(""), text(mon.tree() ? "ARBOL / COMANDO" : "COMANDO") | flex}) | bold | bgcolor(Color::Blue);
        int rows = tableRows(), n = (int)s.snap.rows.size();
        s.ensureVisible(rows);
        Elements lines; lines.push_back(head);
        for (int i = s.scroll; i < std::min(n, s.scroll + rows); ++i) {
            const ProcRow& r = s.snap.rows[(size_t)i];
            std::string usr = r.user.size() > 12 ? r.user.substr(0, 12) : r.user;
            std::string cmd = mon.tree() ? r.treePrefix + r.name : r.cmd;
            if (r.denied) cmd += "  [sin permiso para consultar]";
            if (r.state == 'Z') cmd += "  [terminado sin recoger]";
            if (r.state == 'D') cmd += "  [esperando E/S]";
            if (r.state == 'T') cmd += "  [suspendido]";
            Element row = hbox({cell(std::to_string(r.pid), 6, true), text(" "), cell(std::to_string(r.ppid), 6, true), text(" "), cell(usr, 12), text(" "),
                                cell(std::string(1, r.state), 2),
                                (cell(f("%.1f", r.cpu), 5, true) | color(r.cpu >= 1 ? cpuColor(r.cpu * 4) : Color::Default)), text(" "),
                                cell(f("%.1f", r.mem), 5, true), text(" "), cell(fmtBytes(r.rss), 7, true), text(" "), cell(std::to_string(r.threads), 4, true), text(" "),
                                cell(std::to_string(r.prio), 3, true), text(" "), cell(fmtCpuTime(r.cpuTime), 10, true), text(" "),
                                wide ? cell(r.name, 18) : text(""), wide ? text(" ") : text(""), text(cmd) | flex});
            lines.push_back(stateStyle(row, r.state, r.denied, i == s.selIdx));
        }
        std::string title = " Procesos " + std::to_string(n == 0 ? 0 : s.selIdx + 1) + "/" + std::to_string(n) +
                            (s.snap.hiddenByFilter > 0 ? "  (" + std::to_string(s.snap.hiddenByFilter) + " ocultos por el filtro)" : "") + " ";
        return window(text(title) | bold, vbox(std::move(lines)) | flex) | flex;
    }

    Element renderThreads() {
        auto head = hbox({cell("TID", 8, true), text(" "), cell("E", 2), cell("ESTADO", 14), text(" "), cell("RAZON DE ESPERA", 18), text(" "), cell("PRI.BASE", 8, true),
                          text(" "), cell("PRI.ACT", 8, true), text(" "), cell("TIEMPO CPU", 10, true)}) | bold | bgcolor(Color::Blue);
        int rows = tableRows(), n = (int)s.threads.size();
        s.ensureVisible(rows);
        Elements lines; lines.push_back(head);
        for (int i = s.thScroll; i < std::min(n, s.thScroll + rows); ++i) {
            const RawThread& t = s.threads[(size_t)i];
            Element row = hbox({cell(std::to_string(t.tid), 8, true), text(" "), cell(std::string(1, t.code), 2), cell(t.state, 14), text(" "), cell(t.wait, 18), text(" "),
                                cell(std::to_string(t.basePrio), 8, true), text(" "), cell(std::to_string(t.prio), 8, true), text(" "), cell(fmtCpuTime(t.cpu100ns), 10, true)});
            lines.push_back(stateStyle(row, t.code, false, i == s.thSel));
        }
        if (n == 0) lines.push_back(text("  (el proceso no tiene hilos)") | dim);
        return window(text(" Hilos de " + s.thName + " (PID " + std::to_string(s.thPid) + ") - " + std::to_string(n) + " hilos  |  h/Esc: volver ") | bold, vbox(std::move(lines)) | flex) | flex;
    }

    Element renderHistory() {
        const ProcRow* p = s.current();
        std::vector<float> ph = p ? mon.cpuHistory(p->pid) : std::vector<float>{};
        CpuStats st = cpuStats(ph);
        float top = niceScale(st.max);
        const Summary& sm = s.snap.sum;
        double span = double(Monitor::kHist) * mon.intervalMs() / 1000.0;   // tiempo que cubre el eje X
        auto xAxis = [&] { return hbox({text("      "), text("← " + agoText(span)) | dim, filler(), text("ahora →") | dim}); };
        auto stat = [](const std::string& l, const std::string& v) { return hbox({text(l + " ") | dim, text(v) | bold, text("    ")}); };

        Element procTitle = p ? hbox({text(p->name + " (PID " + std::to_string(p->pid) + ")") | bold | color(Color::Cyan), text("    "),
                                      stat("Ahora", f("%.1f%%", p->cpu)), stat("Promedio", f("%.1f%%", st.avg)), stat("Maximo", f("%.1f%%", st.max)),
                                      stat("Muestras", std::to_string(st.samples) + "/" + std::to_string(Monitor::kHist))})
                              : text("(no hay proceso seleccionado)") | dim;
        Element procGraph = st.samples < 2
            ? vbox({filler(), text("Recogiendo muestras... (una por cada refresco de " + std::to_string(mon.intervalMs()) + " ms)") | dim | center, filler()}) | flex
            : hbox({yAxis(top), separator(), historyGraph(ph, top) | color(Color::Cyan) | flex}) | flex;

        CpuStats ss = cpuStats(sm.cpuHistory);
        Element sysTitle = hbox({text("CPU total de la maquina") | bold, text("    "), stat("Ahora", f("%.1f%%", sm.cpuTotal)),
                                 stat("Promedio", f("%.1f%%", ss.avg)), stat("Maximo", f("%.1f%%", ss.max))});
        Element sysGraph = hbox({yAxis(100), separator(), historyGraph(sm.cpuHistory, 100) | color(cpuColor(sm.cpuTotal)) | flex}) | size(HEIGHT, EQUAL, 7);

        return window(text(" Historico de CPU  |  g/Esc: volver  |  ↑↓: cambiar de proceso ") | bold,
                      vbox({procTitle, procGraph, xAxis(), separator(), sysTitle, sysGraph, xAxis()}) | flex) | flex;
    }

    Element renderHelp() {
        auto k = [](const std::string& a, const std::string& b) { return hbox({text(" " + a) | bold | color(Color::Cyan) | size(WIDTH, EQUAL, 16), text(b)}); };
        return window(text(" Ayuda (cualquier tecla para cerrar) ") | bold, vbox({
            k("q", "salir (restaura la terminal)"), k("↑ ↓ PgUp PgDn", "mover la seleccion  (tambien la rueda del raton)"),
            k("c / m / n / i", "ordenar por CPU / memoria / nombre / PID (CPU y memoria: el mayor primero)"),
            k("/", "filtrar por nombre, comando, usuario o PID (Enter acepta, Esc cancela)"),
            k("t", "vista de arbol padre → hijo"), k("h", "hilos del proceso seleccionado con su estado"),
            k("g", "historico de CPU del proceso seleccionado a lo largo del tiempo (↑↓ cambia de proceso)"),
            k("k", "terminar el proceso (pide confirmacion)"), k("K", "terminar por la FUERZA (confirmacion aparte)"),
            k("s / r", "suspender / reanudar el proceso"), k("p", "ver y cambiar la prioridad"),
            k("f  + -", "intervalo de refresco: escribir ms (250-10000) / subir-bajar 250 ms"),
            text(""),
            text(" Estados:  R corriendo   S durmiendo   D esperando E/S   T detenido   Z terminado sin recoger") | dim,
            text(" Rojo = terminado sin recoger   Amarillo = bloqueado esperando E/S   Magenta = suspendido   Gris = sin permiso") | dim,
            text(" %CPU = tiempo de CPU del proceso entre dos refrescos / tiempo total de CPU del sistema entre esos mismos refrescos") | dim,
        }) | flex) | flex;
    }

    Element renderFooter() {
        using namespace std::chrono;
        Element m = text("");
        if (s.messageFresh())
            m = text(" " + s.message()) | color(s.messageOk() ? Color::Green : Color::Red) | bold;
        Element l2;
        switch (s.input) {
            case InputMode::Filter: l2 = hbox({text(" Filtro: ") | bold | color(Color::Cyan), text(s.buf), text("▌") | blink, filler(), text("Enter aceptar · Esc cancelar ") | dim}); break;
            case InputMode::Interval: l2 = hbox({text(" Intervalo de refresco en ms (250-10000): ") | bold | color(Color::Cyan), text(s.buf), text("▌") | blink, filler(), text("Enter aceptar · Esc cancelar ") | dim}); break;
            case InputMode::Confirm:
                l2 = text(s.pending == Act::Force
                              ? " ¿FORZAR el cierre de " + s.pendName + " (PID " + std::to_string(s.pendPid) + ")? Se pierden los datos sin guardar.  [s] si / otra tecla: cancelar"
                              : " ¿Terminar " + s.pendName + " (PID " + std::to_string(s.pendPid) + ")?  [s] si / otra tecla: cancelar")
                     | bold | color(s.pending == Act::Force ? Color::Red : Color::Yellow);
                break;
            case InputMode::Priority: {
                Elements it; static const char* shortNames[] = {"Idle", "Inferior", "Normal", "Superior", "Alta", "T.Real"};
                it.push_back(text(" Prioridad PID " + std::to_string(s.pendPid) + ": ") | bold);
                for (int i = 0; i < 6; ++i) {
                    Element e = text(" " + std::string(shortNames[i]) + (i == (int)s.prioCur ? "*" : "") + " ");
                    it.push_back(i == s.prioSel ? e | inverted | bold : e);
                }
                it.push_back(text(" ↑↓ Enter Esc (* = actual)") | dim);
                l2 = hbox(std::move(it)); break;
            }
            default:
                l2 = text(" q salir │ ↑↓ mover │ c m n i orden │ / filtro │ t arbol │ h hilos │ g historico │ k/K terminar/forzar │ s/r susp/reanud │ p prior │ f interv │ ? ayuda") | bold | color(Color::Black);
        }
        return vbox({m, s.input == InputMode::None ? (l2 | bgcolor(Color::GrayLight)) : (l2 | bgcolor(Color::Black))});
    }

    Element render() {
        int w = Terminal::Size().dimx;
        Element body = s.input == InputMode::Help ? renderHelp() : s.view == ViewMode::Threads ? renderThreads()
                     : s.view == ViewMode::History ? renderHistory() : renderTable(w);
        return vbox({renderHeader(w), body, renderFooter()});
    }
};

} // namespace

int main() {
#ifdef _WIN32
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#endif
    Monitor mon(makeOsApi());
    std::string err;
    if (!mon.refresh(&err)) { std::fprintf(stderr, "No se pudo leer los procesos: %s\n", err.c_str()); return 1; }

    auto screen = ScreenInteractive::Fullscreen();   // pantalla alternativa a todo el alto y ancho; se restaura al salir
    Ui ui(mon, screen);

    std::atomic<bool> running{true};
    std::thread worker([&] {
        using clock = std::chrono::steady_clock;
        auto last = clock::now() - std::chrono::milliseconds(mon.intervalMs() - 700); // 2a muestra rapida: asi aparece %CPU pronto
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (clock::now() - last >= std::chrono::milliseconds(mon.intervalMs())) {
                last = clock::now();
                mon.refresh();
                screen.PostEvent(Event::Custom);
            }
        }
    });

    auto comp = CatchEvent(Renderer([&] { return ui.render(); }), [&](Event e) { return ui.handle(e); });
    screen.Loop(comp);

    running = false;
    worker.join();
#ifdef _WIN32
    SetConsoleOutputCP(oldCp);
#endif
    return 0;
}
