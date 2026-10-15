// Estado e interaccion de la aplicacion, compartidos por la TUI y la GUI:
// seleccion, vistas (tabla / arbol / hilos), modos de entrada (filtro, intervalo, confirmacion,
// prioridad, ayuda) y mensajes. Cada interfaz solo traduce sus teclas a Key y dibuja este estado.
#pragma once
#include "monitor.h"
#include <chrono>

namespace mon {

enum class KeyType { Char, Up, Down, Left, Right, PageUp, PageDown, Home, End, Enter, Escape, Backspace, WheelUp, WheelDown };
struct Key {
    KeyType type = KeyType::Char;
    std::string ch;            // para KeyType::Char (UTF-8)
    static Key chr(const std::string& c) { Key k; k.ch = c; return k; }
    static Key of(KeyType t) { Key k; k.type = t; return k; }
};

enum class ViewMode { Table, Threads, History };   // History: historico de CPU del proceso seleccionado (tecla g)
enum class InputMode { None, Filter, Interval, Confirm, Priority, Help };
enum class Act { Kill, Force };
enum class KeyResult { Ignored, Handled, Quit };

class Session {
public:
    explicit Session(Monitor& m);

    // --- estado visible para los renderizadores ---
    Monitor& mon;
    Snapshot snap;
    ViewMode view = ViewMode::Table;
    InputMode input = InputMode::None;
    uint32_t selPid = 0;
    int selIdx = 0, scroll = 0;
    uint32_t thPid = 0; std::string thName; std::vector<RawThread> threads; int thSel = 0, thScroll = 0;
    std::string buf;                       // texto en edicion (filtro / intervalo)
    Act pending = Act::Kill; uint32_t pendPid = 0; std::string pendName;
    int prioSel = 2; PrioClass prioCur = PrioClass::Normal;

    // --- operaciones ---
    void reload();                         // vuelve a pedir la instantanea (tras cambiar orden, filtro, etc.)
    void onRefresh();                      // llamar cuando el hilo de refresco termina una lectura
    KeyResult handle(const Key& k, int pageRows);
    void ensureVisible(int rows);          // ajusta `scroll` / `thScroll` para que la seleccion se vea
    void selectIndex(int idx);             // seleccionar con el raton
    const ProcRow* current() const { return snap.rows.empty() ? nullptr : &snap.rows[(size_t)selIdx]; }

    // --- mensaje de confirmacion / error ---
    const std::string& message() const { return msg_; }
    bool messageOk() const { return msgOk_; }
    bool messageFresh(double seconds = 12) const;
    void say(const std::string& t, bool ok);

private:
    std::string msg_; bool msgOk_ = true; std::chrono::steady_clock::time_point msgAt_;
    std::string savedFilter_;
    bool initialSel_ = false;
    void fixSelection();
    void move(int d);
    void loadThreads();
    void doAction(Act a);
    void doSuspendResume(bool susp);
    void askKill(Act a);
    void openPriority();
    void applyPriority();
};

} // namespace mon
