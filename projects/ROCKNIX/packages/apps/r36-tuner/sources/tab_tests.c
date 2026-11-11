/* SPDX-License-Identifier: MIT */
#include "tuner.h"
#include <stdio.h>
#include <string.h>

void tab_tests(Keys k, int top, int bottom) {
    static int sel, first;
    enum { T_STAB, T_CPU, T_RAM, T_GPU, T_HIST };
    int ids[5], n = 0;
    Row rows[5];
    memset(rows, 0, sizeof(rows));
#define ADD(id, lbl, dsc) do { ids[n] = (id); snprintf(rows[n].label, sizeof(rows[n].label), "%s", (lbl)); \
        snprintf(rows[n].desc, sizeof(rows[n].desc), "%s", (dsc)); n++; } while (0)
    ADD(T_STAB, _("Stability test"), _("Combined CPU, memory and GPU load with error detection."));
    ADD(T_CPU, _("CPU benchmark"), _("30 s score to compare configurations."));
    ADD(T_RAM, _("Memory benchmark"), _("Write and copy throughput at maximum frequency. 30 s."));
    if (bench_available_gpu()) ADD(T_GPU, _("GPU benchmark"), _("glmark2, four scenes. About 1 minute."));
    ADD(T_HIST, _("History"), _("Previous results, newest first."));
#undef ADD
    if (k.up) sel = (sel + n - 1) % n;
    if (k.down) sel = (sel + 1) % n;
    if (k.a) {
        switch (ids[sel]) {
        case T_STAB: bench_stability(); break;
        case T_CPU: bench_cpu(); break;
        case T_RAM: bench_ram(); break;
        case T_GPU: bench_gpu(); break;
        case T_HIST: bench_history(); break;
        }
        state_refresh();
    }
    app_chrome();
    int y = top;
    if (app_dtb.net == NET_TRIAL) {
        y += ui_banner(UI_M, y, UW - 2 * UI_M, C_WARN, _("Changes not validated. Run the stability test.")) + G(1);
    }
    ui_list(rows, n, sel, &first, y, bottom);
    const char *hints[5][2] = {{"A", _("Start")}, {"L R", _("Tab")}, {"B", _("Back")}, {NULL, NULL}};
    ui_hints(hints);
}
