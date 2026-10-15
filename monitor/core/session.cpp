#include "session.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mon {

Session::Session(Monitor& m) : mon(m) { reload(); }

bool Session::messageFresh(double seconds) const {
    return !msg_.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - msgAt_).count() < seconds;
}
void Session::say(const std::string& t, bool ok) { msg_ = t; msgOk_ = ok; msgAt_ = std::chrono::steady_clock::now(); }

void Session::reload() { snap = mon.snapshot(); fixSelection(); }

void Session::fixSelection() {
    int n = (int)snap.rows.size();
    if (n == 0) { selIdx = 0; scroll = 0; return; }
    bool found = false;
    for (int i = 0; i < n; ++i) if (snap.rows[(size_t)i].pid == selPid) { selIdx = i; found = true; break; }
    if (!found) selIdx = std::min(std::max(selIdx, 0), n - 1);      // el proceso desaparecio: quedarse cerca
    selPid = snap.rows[(size_t)selIdx].pid;
}

void Session::selectIndex(int idx) {
    int n = (int)snap.rows.size(); if (!n) return;
    if (view == ViewMode::Threads) { thSel = std::min(std::max(0, idx), std::max(0, (int)threads.size() - 1)); return; }
    selIdx = std::min(n - 1, std::max(0, idx)); selPid = snap.rows[(size_t)selIdx].pid;
}

void Session::move(int d) {
    if (view == ViewMode::Threads) { int n = (int)threads.size(); if (n) thSel = std::min(n - 1, std::max(0, thSel + d)); return; }
    selectIndex(selIdx + d);
}

void Session::ensureVisible(int rows) {
    rows = std::max(1, rows);
    if (view == ViewMode::Threads) {
        int n = (int)threads.size();
        if (thSel < thScroll) thScroll = thSel;
        if (thSel >= thScroll + rows) thScroll = thSel - rows + 1;
        thScroll = std::max(0, std::min(thScroll, std::max(0, n - rows)));
    } else {
        int n = (int)snap.rows.size();
        if (selIdx < scroll) scroll = selIdx;
        if (selIdx >= scroll + rows) scroll = selIdx - rows + 1;
        scroll = std::max(0, std::min(scroll, std::max(0, n - rows)));
    }
}

void Session::loadThreads() {
    std::string err; std::vector<RawThread> t;
    if (!mon.threads(thPid, t, err)) {
        say(err.empty() ? "No se pudieron leer los hilos" : err, false);
        view = ViewMode::Table; return;
    }
    threads = std::move(t);
    if (!err.empty()) say(err, false);
    thSel = std::min(thSel, std::max(0, (int)threads.size() - 1));
}

void Session::onRefresh() {
    reload();
    if (!initialSel_ && snap.sum.refreshes >= 2 && !snap.rows.empty()) { initialSel_ = true; selIdx = 0; selPid = snap.rows[0].pid; }
    if (view == ViewMode::Threads) loadThreads();
}

void Session::doAction(Act a) {
    std::string who = pendName + " (PID " + std::to_string(pendPid) + ")";
    ActionResult r = mon.terminate(pendPid, a == Act::Force);
    say(std::string(r.ok ? "OK " : "ERROR ") + (a == Act::Force ? "Terminar por la fuerza" : "Terminar") + " -> " + who + ": " + r.message, r.ok);
    reload();
}
void Session::doSuspendResume(bool susp) {
    auto* p = current(); if (!p) { say("No hay proceso seleccionado", false); return; }
    std::string who = p->name + " (PID " + std::to_string(p->pid) + ")";
    ActionResult r = susp ? mon.suspend(p->pid) : mon.resume(p->pid);
    say(std::string(r.ok ? "OK " : "ERROR ") + (susp ? "Suspender" : "Reanudar") + " -> " + who + ": " + r.message, r.ok);
    reload();
}
void Session::askKill(Act a) {
    auto* p = current(); if (!p) { say("No hay proceso seleccionado", false); return; }
    pending = a; pendPid = p->pid; pendName = p->name; input = InputMode::Confirm;
}
void Session::openPriority() {
    auto* p = current(); if (!p) { say("No hay proceso seleccionado", false); return; }
    ActionResult r = mon.getPriority(p->pid, prioCur);
    if (!r.ok) { say("ERROR Prioridad -> " + p->name + " (PID " + std::to_string(p->pid) + "): " + r.message, false); return; }
    prioSel = (int)prioCur; pendPid = p->pid; pendName = p->name; input = InputMode::Priority;
}
void Session::applyPriority() {
    PrioClass c = (PrioClass)prioSel;
    std::string who = pendName + " (PID " + std::to_string(pendPid) + ")";
    ActionResult r = mon.setPriority(pendPid, c);
    say(std::string(r.ok ? "OK " : "ERROR ") + "Cambiar prioridad -> " + who + ": " + r.message, r.ok);
    reload();
}

KeyResult Session::handle(const Key& k, int pageRows) {
    auto isCh = [&](const char* c) { return k.type == KeyType::Char && k.ch == c; };
    switch (input) {
        case InputMode::Help: input = InputMode::None; return KeyResult::Handled;
        case InputMode::Confirm: {
            bool yes = isCh("s") || isCh("S") || isCh("y") || isCh("Y");
            input = InputMode::None;
            if (yes) doAction(pending);
            else say("Cancelado: no se aplico ninguna accion sobre " + pendName + " (PID " + std::to_string(pendPid) + ")", true);
            return KeyResult::Handled;
        }
        case InputMode::Priority:
            if (k.type == KeyType::Up || k.type == KeyType::Left) prioSel = std::max(0, prioSel - 1);
            else if (k.type == KeyType::Down || k.type == KeyType::Right) prioSel = std::min(5, prioSel + 1);
            else if (k.type == KeyType::Enter) { input = InputMode::None; applyPriority(); }
            else if (k.type == KeyType::Escape) { input = InputMode::None; say("Cancelado el cambio de prioridad", true); }
            return KeyResult::Handled;
        case InputMode::Filter:
            if (k.type == KeyType::Enter) { input = InputMode::None; say(buf.empty() ? "Filtro quitado" : "Filtro: \"" + buf + "\"", true); }
            else if (k.type == KeyType::Escape) { input = InputMode::None; mon.setFilter(savedFilter_); reload(); }
            else if (k.type == KeyType::Backspace) {
                if (!buf.empty()) { size_t i = buf.size() - 1; while (i > 0 && (buf[i] & 0xC0) == 0x80) --i; buf.erase(i); }
                mon.setFilter(buf); reload();
            } else if (k.type == KeyType::Char && !k.ch.empty() && (unsigned char)k.ch[0] >= 32) { buf += k.ch; mon.setFilter(buf); reload(); }
            return KeyResult::Handled;
        case InputMode::Interval:
            if (k.type == KeyType::Enter) {
                input = InputMode::None;
                if (buf.empty()) { say("Intervalo sin cambios", true); return KeyResult::Handled; }
                long v = std::atol(buf.c_str());
                mon.setIntervalMs((int)v);
                say("Intervalo de refresco: " + std::to_string(mon.intervalMs()) + " ms" + (v != mon.intervalMs() ? " (ajustado al rango 250-10000)" : ""), true);
            } else if (k.type == KeyType::Escape) { input = InputMode::None; say("Intervalo sin cambios", true); }
            else if (k.type == KeyType::Backspace) { if (!buf.empty()) buf.pop_back(); }
            else if (k.type == KeyType::Char && k.ch.size() == 1 && std::isdigit((unsigned char)k.ch[0]) && buf.size() < 5) buf += k.ch;
            return KeyResult::Handled;
        case InputMode::None: break;
    }

    switch (k.type) {
        case KeyType::Up: move(-1); return KeyResult::Handled;
        case KeyType::Down: move(1); return KeyResult::Handled;
        case KeyType::WheelUp: move(-3); return KeyResult::Handled;
        case KeyType::WheelDown: move(3); return KeyResult::Handled;
        case KeyType::PageUp: move(-std::max(1, pageRows)); return KeyResult::Handled;
        case KeyType::PageDown: move(std::max(1, pageRows)); return KeyResult::Handled;
        case KeyType::Home: move(-1000000); return KeyResult::Handled;
        case KeyType::End: move(1000000); return KeyResult::Handled;
        case KeyType::Escape: if (view != ViewMode::Table) view = ViewMode::Table; return KeyResult::Handled;
        default: break;
    }
    if (k.type != KeyType::Char) return KeyResult::Ignored;

    if (isCh("q") || isCh("Q")) return KeyResult::Quit;
    if (isCh("?")) { input = InputMode::Help; return KeyResult::Handled; }
    if (isCh("c")) { mon.setSort(SortKey::Cpu); reload(); say("Orden: CPU (el que mas consume, primero)", true); return KeyResult::Handled; }
    if (isCh("m")) { mon.setSort(SortKey::Mem); reload(); say("Orden: memoria (el que mas consume, primero)", true); return KeyResult::Handled; }
    if (isCh("n")) { mon.setSort(SortKey::Name); reload(); say("Orden: nombre", true); return KeyResult::Handled; }
    if (isCh("i")) { mon.setSort(SortKey::Pid); reload(); say("Orden: PID", true); return KeyResult::Handled; }
    if (isCh("t")) {
        mon.setTree(!mon.tree()); view = ViewMode::Table; reload();
        say(mon.tree() ? "Vista de arbol: padre -> hijo" : "Vista de tabla plana", true); return KeyResult::Handled;
    }
    if (isCh("/")) { input = InputMode::Filter; savedFilter_ = mon.filter(); buf = savedFilter_; return KeyResult::Handled; }
    if (isCh("f")) { input = InputMode::Interval; buf.clear(); return KeyResult::Handled; }
    if (isCh("+") || isCh("=")) { mon.setIntervalMs(mon.intervalMs() + 250); say("Intervalo de refresco: " + std::to_string(mon.intervalMs()) + " ms", true); return KeyResult::Handled; }
    if (isCh("-") || isCh("_")) { mon.setIntervalMs(mon.intervalMs() - 250); say("Intervalo de refresco: " + std::to_string(mon.intervalMs()) + " ms", true); return KeyResult::Handled; }
    if (isCh("h") || isCh("H")) {
        if (view == ViewMode::Threads) { view = ViewMode::Table; return KeyResult::Handled; }
        auto* p = current(); if (!p) { say("No hay proceso seleccionado", false); return KeyResult::Handled; }
        thPid = p->pid; thName = p->name; thSel = 0; thScroll = 0; view = ViewMode::Threads; loadThreads(); return KeyResult::Handled;
    }
    if (isCh("g") || isCh("G")) {      // historico de CPU del proceso seleccionado (las flechas cambian de proceso)
        if (view == ViewMode::History) { view = ViewMode::Table; return KeyResult::Handled; }
        if (!current()) { say("No hay proceso seleccionado", false); return KeyResult::Handled; }
        view = ViewMode::History; return KeyResult::Handled;
    }
    if (isCh("k")) { askKill(Act::Kill); return KeyResult::Handled; }
    if (isCh("K")) { askKill(Act::Force); return KeyResult::Handled; }
    if (isCh("s")) { doSuspendResume(true); return KeyResult::Handled; }
    if (isCh("r")) { doSuspendResume(false); return KeyResult::Handled; }
    if (isCh("p")) { openPriority(); return KeyResult::Handled; }
    return KeyResult::Ignored;
}

} // namespace mon
