// Interfaz grafica (raylib). Usa exactamente el mismo core y la misma Session que la TUI:
// aqui solo se traducen teclas/raton a Key y se dibuja el estado.
#include "../core/session.h"
#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace mon;

namespace {

const Color kBg{22, 24, 30, 255}, kPanel{32, 35, 43, 255}, kLine{70, 75, 90, 255}, kText{222, 224, 230, 255},
    kDim{140, 146, 158, 255}, kCyan{90, 205, 235, 255}, kGreen{95, 205, 125, 255}, kYellow{242, 202, 72, 255},
    kRed{242, 95, 95, 255}, kMagenta{205, 130, 235, 255}, kHead{45, 85, 150, 255}, kSel{62, 84, 128, 255};

Font gFont;
const float kSize = 16.f;

float TW(const std::string& s) { return MeasureTextEx(gFont, s.c_str(), kSize, 0).x; }
void T(const std::string& s, float x, float y, Color c) { DrawTextEx(gFont, s.c_str(), {x, y}, kSize, 0, c); }
void TR(const std::string& s, float xr, float y, Color c) { T(s, xr - TW(s), y, c); }
std::string f1(const char* fmt, double v) { char b[64]; std::snprintf(b, sizeof b, fmt, v); return b; }

Color levelColor(double pct) { return pct >= 60 ? kRed : pct >= 25 ? kYellow : kGreen; }

void replaceAll(std::string& s, const std::string& a, const std::string& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
}
std::string asciiTree(std::string s) {      // la fuente de raylib no trae los caracteres de caja
    replaceAll(s, "├─ ", "|- "); replaceAll(s, "└─ ", "`- "); replaceAll(s, "│  ", "|  ");
    return s;
}
std::string utf8(int cp) {
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    return s;
}

void bar(float x, float y, float w, float h, double pct, Color c) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, Color{50, 54, 66, 255});
    DrawRectangle((int)x, (int)y, (int)(w * std::min(1.0, std::max(0.0, pct / 100.0))), (int)h, c);
}

void history(float x, float y, float w, float h, const std::vector<float>& v, float scaleMax, Color c) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, Color{26, 28, 35, 255});
    DrawRectangleLines((int)x, (int)y, (int)w, (int)h, kLine);
    if (v.empty()) return;
    float step = w / 120.f;
    for (size_t i = 0; i < v.size(); ++i) {
        float px = x + w - step * (float)(v.size() - i);
        float bh = std::min(1.f, v[i] / scaleMax) * (h - 2);
        DrawRectangle((int)px, (int)(y + h - 1 - bh), std::max(1, (int)step - 1), (int)bh, c);
    }
}

// Grafico grande con eje Y (tope, mitad, 0) y lineas guia; usa history() para las barras.
void plotWithAxes(float x, float y, float w, float h, const std::vector<float>& v, float top, Color c) {
    history(x, y, w, h, v, top, c);
    for (int i = 0; i <= 2; ++i) {
        float gy = y + (h - 1) * i / 2.f;
        if (i == 1) DrawLine((int)x, (int)gy, (int)(x + w), (int)gy, Color{60, 64, 78, 255});
        TR(f1("%.0f%%", top * (2 - i) / 2.f), x - 6, gy - (i == 0 ? 0 : i == 2 ? 14 : 7), kDim);
    }
}

std::string agoText(double secs) { return secs >= 60 ? "hace ~" + f1("%.0f", secs / 60) + " min" : "hace ~" + f1("%.0f", secs) + " s"; }

const char* kPrioShort[] = {"Idle (baja)", "Inferior a normal", "Normal", "Superior a normal", "Alta", "Tiempo real"};

} // namespace

int main() {
    Monitor mon(makeOsApi());
    std::string err;
    if (!mon.refresh(&err)) { std::fprintf(stderr, "No se pudo leer los procesos: %s\n", err.c_str()); return 1; }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);   // maximizar / redimensionar con el raton, sin cortes
    InitWindow(1280, 720, "Monitor de procesos");
    SetWindowMinSize(820, 480);
    SetExitKey(KEY_NULL);                                       // Esc solo cancela; se sale con Q o cerrando la ventana
    SetTargetFPS(60);

    int cps[224]; for (int i = 0; i < 224; ++i) cps[i] = 32 + i;
    gFont = LoadFontEx("C:/Windows/Fonts/consola.ttf", 16, cps, 224);
    if (gFont.texture.id == 0) gFont = GetFontDefault();
    else SetTextureFilter(gFont.texture, TEXTURE_FILTER_BILINEAR);

    Session s(mon);
    std::atomic<bool> running{true}, dirty{false};
    std::thread worker([&] {
        using clock = std::chrono::steady_clock;
        auto last = clock::now() - std::chrono::milliseconds(mon.intervalMs() - 700);
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            if (clock::now() - last >= std::chrono::milliseconds(mon.intervalMs())) {
                last = clock::now(); mon.refresh(); dirty = true;
            }
        }
    });

    bool quit = false;
#ifdef MONITOR_GUI_SELFTEST     // solo para pruebas automaticas sin pantalla: guarda una captura y sale
    int selfFrames = 0;
#endif
    while (!quit && !WindowShouldClose()) {
        const int W = GetScreenWidth(), H = GetScreenHeight();
        const int headerH = 124, footerH = 50, colH = 24, rowH = 20;
        const int tableY = headerH + 8;
        const int rowsTop = tableY + 26 + colH;
        const int rows = std::max(1, (H - footerH - rowsTop - 6) / rowH);

        if (dirty.exchange(false)) s.onRefresh();

        // ---------- entrada ----------
        auto send = [&](const Key& k) { if (s.handle(k, rows) == KeyResult::Quit) quit = true; };
        for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) send(Key::chr(utf8(c)));
        auto pr = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };
        if (pr(KEY_UP)) send(Key::of(KeyType::Up));
        if (pr(KEY_DOWN)) send(Key::of(KeyType::Down));
        if (pr(KEY_LEFT)) send(Key::of(KeyType::Left));
        if (pr(KEY_RIGHT)) send(Key::of(KeyType::Right));
        if (pr(KEY_PAGE_UP)) send(Key::of(KeyType::PageUp));
        if (pr(KEY_PAGE_DOWN)) send(Key::of(KeyType::PageDown));
        if (IsKeyPressed(KEY_HOME)) send(Key::of(KeyType::Home));
        if (IsKeyPressed(KEY_END)) send(Key::of(KeyType::End));
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) send(Key::of(KeyType::Enter));
        if (IsKeyPressed(KEY_ESCAPE)) send(Key::of(KeyType::Escape));
        if (pr(KEY_BACKSPACE)) send(Key::of(KeyType::Backspace));
        if (IsKeyPressed(KEY_F1)) send(Key::chr("?"));
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        float wheel = GetMouseWheelMove();
        if (wheel > 0) send(Key::of(KeyType::WheelUp));
        else if (wheel < 0) send(Key::of(KeyType::WheelDown));
        Vector2 mp = GetMousePosition();
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && s.input == InputMode::None && s.view != ViewMode::History && mp.y >= rowsTop && mp.y < rowsTop + rows * rowH) {
            int base = s.view == ViewMode::Threads ? s.thScroll : s.scroll;
            s.selectIndex(base + (int)((mp.y - rowsTop) / rowH));
        }
        s.ensureVisible(rows);

        // ---------- dibujo ----------
        const Summary& sm = s.snap.sum;
        BeginDrawing();
        ClearBackground(kBg);

        // panel de resumen
        DrawRectangle(8, 8, W - 16, headerH - 8, kPanel);
        DrawRectangleLines(8, 8, W - 16, headerH - 8, kLine);
        float x = 18, y = 14;
        auto stat = [&](const std::string& l, int v, Color c) { T(l, x, y, kDim); x += TW(l) + 6; std::string n = std::to_string(v); T(n, x, y, c); x += TW(n) + 22; };
        stat("Procesos", sm.total, kText); stat("Corriendo", sm.running, kGreen); stat("Bloq. E/S", sm.blocked, kYellow);
        stat("Detenidos", sm.stopped, kMagenta); stat("Zombis", sm.zombie, kRed); stat("Dormidos", sm.sleeping, kDim);
        float graphW = W >= 1000 ? 440.f : 0.f;
        float leftW = W - 16 - 20 - (graphW > 0 ? graphW + 20 : 0);
        float barW = std::max(120.f, leftW - 290.f);
        T("CPU", 18, y + 24, kText); bar(58, y + 26, barW, 14, sm.cpuTotal, levelColor(sm.cpuTotal));
        T(f1("%5.1f", sm.cpuTotal) + "%", 66 + barW, y + 24, kText);
        T("Carga " + f1("%.2f ", sm.load1) + f1("%.2f ", sm.load5) + f1("%.2f", sm.load15), 66 + barW + 70, y + 24, kDim);
        double mpct = sm.memTotal ? 100.0 * double(sm.memUsed) / double(sm.memTotal) : 0;
        T("MEM", 18, y + 48, kText); bar(58, y + 50, barW, 14, mpct, levelColor(mpct));
        T(f1("%5.1f", mpct) + "%", 66 + barW, y + 48, kText);
        T(fmtBytes(sm.memUsed) + " / " + fmtBytes(sm.memTotal) + "  disp. " + fmtBytes(sm.memAvail), 66 + barW + 70, y + 48, kDim);
        std::string flt = mon.filter();
        T(std::string("Orden ") + sortName(mon.sort()) + "   Vista " + (s.view == ViewMode::Threads ? "hilos" : s.view == ViewMode::History ? "historico" : mon.tree() ? "arbol" : "tabla") +
              "   Refresco " + std::to_string(mon.intervalMs()) + " ms   Filtro " + (flt.empty() ? "-" : flt) + "   Activo " + fmtUptime(sm.uptimeMs),
          18, y + 74, kCyan);
        if (graphW > 0) {
            float gx = W - 16 - graphW - 6, gw = (graphW - 10) / 2;
            T("CPU total " + f1("%.0f", sm.cpuTotal) + "%", gx, y, kDim);
            history(gx, y + 20, gw, 70, sm.cpuHistory, 100.f, levelColor(sm.cpuTotal));
            std::vector<float> ph; float pmax = 10.f; std::string title = "(sin proceso)";
            if (auto* p = s.current()) {
                ph = mon.cpuHistory(p->pid); for (float v : ph) pmax = std::max(pmax, v);
                title = p->name + " " + f1("%.1f", p->cpu) + "%";
            }
            if (title.size() > 24) title = title.substr(0, 24);
            T("Hist. " + title, gx + gw + 10, y, kDim);
            history(gx + gw + 10, y + 20, gw, 70, ph, pmax, kCyan);
            T("escala 0-" + f1("%.0f", pmax) + "%", gx + gw + 10, y + 92, kDim);
        }

        // tabla / hilos / ayuda
        auto cmdOf = [&](const ProcRow& r) {
            std::string c = mon.tree() ? asciiTree(r.treePrefix) + r.name : r.cmd;
            if (r.denied) c += "   [sin permiso para consultar]";
            if (r.state == 'Z') c += "   [terminado sin recoger]";
            if (r.state == 'D') c += "   [esperando E/S]";
            if (r.state == 'T') c += "   [suspendido]";
            return c;
        };
        if (s.input == InputMode::Help) {
            DrawRectangle(8, tableY, W - 16, H - tableY - footerH, kPanel);
            DrawRectangleLines(8, tableY, W - 16, H - tableY - footerH, kLine);
            const char* L[] = {
                "AYUDA (cualquier tecla para cerrar)", "",
                "q  salir                      Flechas / PgUp / PgDn / Home / End / rueda: mover la seleccion (o clic en una fila)",
                "c m n i  ordenar por CPU, memoria, nombre o PID (CPU y memoria: el que mas consume, primero)",
                "/  filtrar por nombre, comando, usuario o PID (Enter acepta, Esc cancela)",
                "t  vista de arbol padre -> hijo          h  hilos del proceso seleccionado con su estado",
                "g  historico de CPU del proceso seleccionado a lo largo del tiempo (flechas: cambiar de proceso)",
                "k  terminar (pide confirmacion)          K  terminar por la FUERZA (confirmacion aparte)",
                "s  suspender            r  reanudar      p  ver y cambiar la prioridad",
                "f  intervalo de refresco (escribir ms, 250-10000)       + / -  subir o bajar 250 ms      F11 pantalla completa", "",
                "Estados: R corriendo   S durmiendo   D esperando E/S (amarillo)   T detenido (magenta)   Z terminado sin recoger (rojo)",
                "%CPU = tiempo de CPU del proceso entre dos refrescos / tiempo total de CPU del sistema entre esos mismos refrescos"};
            for (int i = 0; i < 13; ++i) T(L[i], 24, (float)(tableY + 14 + i * 22), i == 0 ? kCyan : kText);
        } else if (s.view == ViewMode::History) {
            const ProcRow* p = s.current();
            std::vector<float> ph = p ? mon.cpuHistory(p->pid) : std::vector<float>{};
            CpuStats st = cpuStats(ph), ss = cpuStats(sm.cpuHistory);
            float top = niceScale(st.max);
            double span = double(Monitor::kHist) * mon.intervalMs() / 1000.0;      // tiempo que cubre el eje X
            T("Historico de CPU   [g / Esc: volver   flechas: cambiar de proceso]", 12, (float)tableY, kCyan);
            const int top0 = tableY + 28, bottom = H - footerH - 8, avail = bottom - top0;
            const int sysH = std::max(60, avail / 4), procH = std::max(60, avail - sysH - 104);
            const float gx = 64, gw = (float)W - 16 - gx - 8;
            auto stats = [&](float sx, float sy, double now, const CpuStats& c, bool samples) {
                std::string t = "Ahora " + f1("%.1f%%", now) + "    Promedio " + f1("%.1f%%", c.avg) + "    Maximo " + f1("%.1f%%", c.max);
                if (samples) t += "    Muestras " + std::to_string(c.samples) + "/" + std::to_string(Monitor::kHist);
                T(t, sx, sy, kText);
            };
            // proceso seleccionado
            std::string pt = p ? p->name + " (PID " + std::to_string(p->pid) + ")" : "(no hay proceso seleccionado)";
            T(pt, 14, (float)top0, kCyan);
            if (p) stats(14 + TW(pt) + 30, (float)top0, p->cpu, st, true);
            float py = (float)top0 + 24;
            if (st.samples < 2) {
                DrawRectangleLines((int)gx, (int)py, (int)gw, procH, kLine);
                T("Recogiendo muestras... (una por cada refresco de " + std::to_string(mon.intervalMs()) + " ms)", gx + 16, py + procH / 2.f - 8, kDim);
            } else plotWithAxes(gx, py, gw, (float)procH, ph, top, kCyan);
            T("<- " + agoText(span), gx, py + procH + 4, kDim); TR("ahora ->", gx + gw, py + procH + 4, kDim);
            // CPU total
            float sy = py + procH + 32;
            T("CPU total de la maquina", 14, sy, kText);
            stats(14 + TW("CPU total de la maquina") + 30, sy, sm.cpuTotal, ss, false);
            plotWithAxes(gx, sy + 24, gw, (float)sysH, sm.cpuHistory, 100.f, levelColor(sm.cpuTotal));
            T("<- " + agoText(span), gx, sy + 24 + sysH + 4, kDim); TR("ahora ->", gx + gw, sy + 24 + sysH + 4, kDim);
        } else if (s.view == ViewMode::Threads) {
            T("Hilos de " + s.thName + " (PID " + std::to_string(s.thPid) + ") - " + std::to_string(s.threads.size()) + " hilos   [h / Esc: volver]", 12, (float)tableY, kCyan);
            DrawRectangle(8, tableY + 24, W - 16, colH, kHead);
            T("TID", 14, tableY + 28, kText); T("E", 110, tableY + 28, kText); T("ESTADO", 140, tableY + 28, kText);
            T("RAZON DE ESPERA", 290, tableY + 28, kText); TR("PRI.BASE", 560, tableY + 28, kText); TR("PRI.ACT", 650, tableY + 28, kText); TR("TIEMPO CPU", 780, tableY + 28, kText);
            int n = (int)s.threads.size();
            for (int i = s.thScroll; i < std::min(n, s.thScroll + rows); ++i) {
                const RawThread& t = s.threads[(size_t)i];
                float ry = (float)(rowsTop + (i - s.thScroll) * rowH);
                if (i == s.thSel) DrawRectangle(8, (int)ry, W - 16, rowH, kSel);
                Color c = t.code == 'D' ? kYellow : t.code == 'T' ? kMagenta : t.code == 'Z' ? kRed : kText;
                TR(std::to_string(t.tid), 90, ry + 2, c); T(std::string(1, t.code), 110, ry + 2, c); T(t.state, 140, ry + 2, c); T(t.wait, 290, ry + 2, c);
                TR(std::to_string(t.basePrio), 560, ry + 2, c); TR(std::to_string(t.prio), 650, ry + 2, c); TR(fmtCpuTime(t.cpu100ns), 780, ry + 2, c);
            }
        } else {
            std::string title = "Procesos " + std::to_string(s.snap.rows.empty() ? 0 : s.selIdx + 1) + "/" + std::to_string(s.snap.rows.size()) +
                                (s.snap.hiddenByFilter > 0 ? "  (" + std::to_string(s.snap.hiddenByFilter) + " ocultos por el filtro)" : "");
            T(title, 12, (float)tableY, kCyan);
            DrawRectangle(8, tableY + 24, W - 16, colH, kHead);
            const float cy = (float)(tableY + 28);
            const bool wide = W >= 1100;
            TR("PID", 66, cy, kText); TR("PPID", 122, cy, kText); T("USUARIO", 134, cy, kText); T("E", 252, cy, kText);
            TR("%CPU", 322, cy, kText); TR("%MEM", 382, cy, kText); TR("RSS", 452, cy, kText); TR("HIL", 500, cy, kText);
            TR("PRI", 540, cy, kText); TR("TIEMPO CPU", 640, cy, kText);
            if (wide) T("PROGRAMA", 656, cy, kText);
            const float cmdX = wide ? 820.f : 656.f;
            T(mon.tree() ? "ARBOL / COMANDO" : "COMANDO", cmdX, cy, kText);
            int n = (int)s.snap.rows.size();
            for (int i = s.scroll; i < std::min(n, s.scroll + rows); ++i) {
                const ProcRow& r = s.snap.rows[(size_t)i];
                float ry = (float)(rowsTop + (i - s.scroll) * rowH);
                if (i == s.selIdx) DrawRectangle(8, (int)ry, W - 16, rowH, kSel);
                Color c = r.state == 'Z' ? kRed : r.state == 'D' ? kYellow : r.state == 'T' ? kMagenta : r.denied ? kDim : kText;
                TR(std::to_string(r.pid), 66, ry + 2, c); TR(std::to_string(r.ppid), 122, ry + 2, c);
                T(r.user.substr(0, 12), 134, ry + 2, c); T(std::string(1, r.state), 252, ry + 2, c);
                TR(f1("%.1f", r.cpu), 322, ry + 2, r.state == 'Z' || r.state == 'D' || r.state == 'T' ? c : (r.cpu >= 1 ? levelColor(r.cpu * 4) : c));
                TR(f1("%.1f", r.mem), 382, ry + 2, c); TR(fmtBytes(r.rss), 452, ry + 2, c); TR(std::to_string(r.threads), 500, ry + 2, c);
                TR(std::to_string(r.prio), 540, ry + 2, c); TR(fmtCpuTime(r.cpuTime), 640, ry + 2, c);
                if (wide) { BeginScissorMode(656, (int)ry, 160, rowH); T(r.name, 656, ry + 2, c); EndScissorMode(); }
                BeginScissorMode((int)cmdX, (int)ry, std::max(1, W - 12 - (int)cmdX), rowH);
                T(cmdOf(r), cmdX, ry + 2, c);
                EndScissorMode();
            }
            if (n > rows) {   // barra de desplazamiento
                float th = std::max(20.f, (float)(rows * rowH) * rows / n);
                float ty = rowsTop + (float)(rows * rowH - th) * (n - rows > 0 ? (float)s.scroll / (n - rows) : 0.f);
                DrawRectangle(W - 12, rowsTop, 4, rows * rowH, Color{40, 44, 54, 255});
                DrawRectangle(W - 12, (int)ty, 4, (int)th, kDim);
            }
        }

        // pie: mensaje + ayuda o linea de entrada
        DrawRectangle(0, H - footerH, W, footerH, Color{28, 30, 37, 255});
        if (s.messageFresh()) T(s.message(), 12, (float)(H - footerH + 6), s.messageOk() ? kGreen : kRed);
        float ly = (float)(H - 24);
        bool blink = ((int)(GetTime() * 2)) % 2 == 0;
        switch (s.input) {
            case InputMode::Filter: T("Filtro: " + s.buf + (blink ? "_" : " ") + "     (Enter aceptar | Esc cancelar)", 12, ly, kCyan); break;
            case InputMode::Interval: T("Intervalo de refresco en ms (250-10000): " + s.buf + (blink ? "_" : " ") + "     (Enter aceptar | Esc cancelar)", 12, ly, kCyan); break;
            case InputMode::Confirm: T(s.pending == Act::Force
                    ? "FORZAR el cierre de " + s.pendName + " (PID " + std::to_string(s.pendPid) + ")? Se pierden datos sin guardar.   [S] si / otra tecla: cancelar"
                    : "Terminar " + s.pendName + " (PID " + std::to_string(s.pendPid) + ")?   [S] si / otra tecla: cancelar", 12, ly, s.pending == Act::Force ? kRed : kYellow); break;
            case InputMode::Priority: T("Prioridad: Flechas elegir | Enter aplicar | Esc cancelar", 12, ly, kCyan); break;
            default: T("Q salir | flechas mover | c m n i orden | / filtro | t arbol | h hilos | g historico | k/K terminar/forzar | s/r suspender/reanudar | p prioridad | f intervalo | ? ayuda", 12, ly, kDim);
        }

        // ventanas emergentes
        if (s.input == InputMode::Confirm) {
            Color bc = s.pending == Act::Force ? kRed : kYellow;
            int bw = 620, bh = 120, bx = (W - bw) / 2, by = (H - bh) / 2;
            DrawRectangle(bx, by, bw, bh, kPanel); DrawRectangleLinesEx(Rectangle{(float)bx, (float)by, (float)bw, (float)bh}, 2, bc);
            T(s.pending == Act::Force ? "TERMINAR POR LA FUERZA" : "TERMINAR PROCESO", (float)bx + 20, (float)by + 16, bc);
            T(s.pendName + "  (PID " + std::to_string(s.pendPid) + ")", (float)bx + 20, (float)by + 44, kText);
            T("[S] confirmar        [cualquier otra tecla] cancelar", (float)bx + 20, (float)by + 80, kDim);
        } else if (s.input == InputMode::Priority) {
            int bw = 360, bh = 40 + 6 * 24 + 16, bx = (W - bw) / 2, by = (H - bh) / 2;
            DrawRectangle(bx, by, bw, bh, kPanel); DrawRectangleLinesEx(Rectangle{(float)bx, (float)by, (float)bw, (float)bh}, 2, kCyan);
            T("Prioridad de " + s.pendName + " (PID " + std::to_string(s.pendPid) + ")", (float)bx + 14, (float)by + 10, kCyan);
            for (int i = 0; i < 6; ++i) {
                float ry = (float)(by + 40 + i * 24);
                if (i == s.prioSel) DrawRectangle(bx + 8, (int)ry, bw - 16, 22, kSel);
                T(std::string(kPrioShort[i]) + (i == (int)s.prioCur ? "   (actual)" : ""), (float)bx + 18, ry + 3, kText);
            }
        }
        EndDrawing();
#ifdef MONITOR_GUI_SELFTEST
        if (selfFrames == 20) send(Key::chr("h"));
        if (selfFrames == 60) { TakeScreenshot("gui_threads.png"); send(Key::chr("h")); send(Key::chr("t")); }
        if (++selfFrames == 120) { TakeScreenshot("gui_tree.png"); quit = true; }
#endif
    }

    running = false;
    worker.join();
    CloseWindow();
    return 0;
}
