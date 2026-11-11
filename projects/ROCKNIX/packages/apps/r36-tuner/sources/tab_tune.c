/* SPDX-License-Identifier: MIT
 * Second tab: every setting that works without restarting, changed in place with left and right. */
#include "tuner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { R_CPU_GOV, R_CPU_MAX, R_TURBO, R_GPU_MODE, R_GPU_MAX, R_RAM_GOV, R_RAM_MAX, R_PERF, R_LANG } RowId;

#define MAXV 16
static long long read_list(const char *path, long long *out, int max) {
    char b[500], *t[MAXV];
    sys_read(path, b, sizeof(b));
    int n = sys_split(b, t, max < MAXV ? max : MAXV);
    for (int i = 0; i < n; i++) out[i] = atoll(t[i]);
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j] < out[i]) { long long x = out[i]; out[i] = out[j]; out[j] = x; }
    return n;
}

/* Next value of a list, or the same one at the ends */
static long long step_ll(const long long *v, int n, long long cur, int dir) {
    int at = 0;
    for (int i = 0; i < n; i++) if (v[i] <= cur) at = i;
    at += dir;
    if (at < 0) at = 0;
    if (at >= n) at = n - 1;
    return n ? v[at] : cur;
}

static void limit_step(const char *avail, const char *maxf, int dir) {
    long long v[MAXV], cur = sys_ll(maxf);
    int n = (int)read_list(avail, v, MAXV);
    long long nv = step_ll(v, n, cur, dir);
    if (nv != cur) {
        char b[32];
        snprintf(b, sizeof(b), "%lld", nv);
        sys_write(maxf, b);
        plat->save_limits();
        profile_apply(PROFILE_CUSTOM);
    }
}

static void gov_step(int dir) {
    char b[300], cur[32], *t[12];
    sys_read(CPU_POLICY "/scaling_available_governors", b, sizeof(b));
    sys_read(CPU_POLICY "/scaling_governor", cur, sizeof(cur));
    int n = sys_split(b, t, 12), at = 0;
    for (int i = 0; i < n; i++) if (!strcmp(t[i], cur)) at = i;
    if (at + dir < 0 || at + dir >= n) return;
    plat->set_cpu_gov(t[at + dir]);
    profile_apply(PROFILE_CUSTOM);
}

static const char *const MODES[] = {"auto", "performance", "powersave"};

static int mode_of(const char *gov) {
    if (strstr(gov, "performance")) return 1;
    if (strstr(gov, "powersave")) return 2;
    return 0;
}

static const char *mode_name(int m) { return m == 1 ? _("Performance") : m == 2 ? _("Power saving") : _("On demand"); }

static const char *gov_label(const char *g) {
    if (strstr(g, "ondemand")) return _("On demand");
    if (strstr(g, "performance")) return _("Performance");
    if (strstr(g, "powersave")) return _("Power saving");
    if (strstr(g, "conservative")) return _("Conservative");
    if (strstr(g, "userspace")) return _("Manual");
    if (strstr(g, "interactive")) return _("Interactive");
    if (strstr(g, "schedutil")) return "Schedutil";
    return g;
}

static void mode_step(RowId r, const char *gov, int dir) {
    int m = mode_of(gov) + dir;
    if (m < 0 || m > 2) return;
    if (r == R_GPU_MODE) plat->set_gpu_mode(MODES[m]);
    else plat->set_ram_gov(m ? MODES[m] : "ondemand");
    profile_apply(PROFILE_CUSTOM);
}

static void turbo_toggle(void) {
    if (st.boost == 1) {
        sys_root("bash -c '. %s; set_setting enable.turbo-mode 0'", rp("/etc/profile"));
        sys_write(BOOST_FILE, "0");
    } else {
        const char *warn[1] = {_("1512 MHz on all four cores has caused resets on some units. Turbo stays enabled after restart.")};
        if (!page_confirm(_("Enable Turbo?"), _("The CPU will run at up to 1512 MHz at 1350 mV."), warn, 1,
                          _("Enable"), _("Cancel"), 1)) return;
        sys_write(BOOST_FILE, "1");
        sys_root("bash -c '. %s; set_setting enable.turbo-mode 1'", rp("/etc/profile"));
    }
    state_refresh();
}

/* BSP kernels list clocks above 1296 MHz that the silicon does not reach. Measures the same loop at both clocks. */
#define OC_CACHE "/tmp/r36_oc_detected"
static long long quick_loop(Uint32 ms) {
    volatile unsigned a = 1, b = 2;
    long long n = 0;
    Uint32 t0 = SDL_GetTicks();
    while (SDL_GetTicks() - t0 < ms) {
        for (int i = 0; i < 1000000; i++) { a = a * 1664525u + 1013904223u; b = b * 1664525u + 1013904223u; }
        n += 2;
    }
    return n;
}

/* 1 real, 0 fake, -1 unknown */
static int oc_is_real(int khz) {
    char b[16];
    if (sys_read(OC_CACHE, b, sizeof(b)) == 0 && (b[0] == '0' || b[0] == '1')) return b[0] - '0';
    sys_cap(b, sizeof(b), "dmesg 2>/dev/null | grep -c '\\[oga-avs\\]'");
    if (atoi(b) > 0) { sys_write(OC_CACHE, "1\n"); return 1; }
    if (khz <= CPU_STOCK_MAX_MHZ * 1000) return -1;
    page_busy(_("CPU maximum frequency"), _("Checking whether this frequency is real… (8 s)"));
    char gov[32], v[32];
    sys_read(CPU_POLICY "/scaling_governor", gov, sizeof(gov));
    sys_write(CPU_POLICY "/scaling_governor", "performance");
    snprintf(v, sizeof(v), "%d", CPU_STOCK_MAX_MHZ * 1000);
    sys_write(CPU_POLICY "/scaling_max_freq", v);
    quick_loop(500);
    long long base = quick_loop(2500);
    snprintf(v, sizeof(v), "%d", khz);
    sys_write(CPU_POLICY "/scaling_max_freq", v);
    quick_loop(500);
    long long fast = quick_loop(2500);
    sys_write(CPU_POLICY "/scaling_governor", gov);
    double ratio = base > 0 ? (double)fast / (double)base : 1.0;
    double expect = (double)(khz / 1000) / CPU_STOCK_MAX_MHZ;
    int real = ratio > 1.0 + (expect - 1.0) * 0.5;
    sys_write(OC_CACHE, real ? "1\n" : "0\n");
    return real;
}

void tab_tune(Keys k, int top, int bottom) {
    static int sel, first;
    RowId ids[10];
    int n = 0;
    ids[n++] = R_CPU_GOV;
    ids[n++] = R_CPU_MAX;
    if (plat->has_boost && st.boost >= 0) ids[n++] = R_TURBO;
    ids[n++] = R_GPU_MODE;
    ids[n++] = R_GPU_MAX;
    if (!plat->joint_ram_gov) ids[n++] = R_RAM_GOV;
    ids[n++] = R_RAM_MAX;
    if (plat->has_perfmode) ids[n++] = R_PERF;
    if (plat->has_lang_menu) ids[n++] = R_LANG;
    if (sel >= n) sel = n - 1;

    if (k.up) sel = (sel + n - 1) % n;
    if (k.down) sel = (sel + 1) % n;
    int dir = k.left ? -1 : k.right ? 1 : 0;
    RowId id = ids[sel];
    if (dir) {
        switch (id) {
        case R_CPU_GOV: gov_step(dir); break;
        case R_CPU_MAX: {
            limit_step(CPU_POLICY "/scaling_available_frequencies", CPU_POLICY "/scaling_max_freq", dir);
            sys_root("rm -f '%s'", rp(OC_CACHE));
            break;
        }
        case R_GPU_MODE: mode_step(id, st.gpu_gov, dir); break;
        case R_GPU_MAX: limit_step(GPU_DEVFREQ "/available_frequencies", GPU_DEVFREQ "/max_freq", dir); break;
        case R_RAM_GOV: mode_step(id, st.ram_gov, dir); break;
        case R_RAM_MAX: limit_step(DMC_DEVFREQ "/available_frequencies", DMC_DEVFREQ "/max_freq", dir); break;
        case R_PERF: {
            static const char *const vals[3] = {"auto", "1", "0"};
            char cur[16];
            plat->perfmode_get(cur, sizeof(cur));
            int at = !strcmp(cur, "1") ? 1 : !strcmp(cur, "0") ? 2 : 0;
            at += dir;
            if (at >= 0 && at <= 2) { plat->perfmode_set(vals[at]); profile_apply(PROFILE_CUSTOM); }
            break;
        }
        case R_LANG: i18n_set(strcmp(i18n_get(), "es") ? "es" : "en"); break;
        case R_TURBO: turbo_toggle(); break;
        }
        state_refresh();
    }
    if (k.a && id == R_TURBO) turbo_toggle();

    app_chrome();
    Row rows[10];
    memset(rows, 0, sizeof(rows));
    const char *desc = "";
    int w = UW - 2 * UI_M, dh = 0;
    for (int i = 0; i < n; i++) {
        Row *r = &rows[i];
        const char *d = "";
        switch (ids[i]) {
        case R_CPU_GOV:
            snprintf(r->label, sizeof(r->label), "%s", plat->joint_ram_gov ? _("CPU and memory governor") : _("CPU governor"));
            snprintf(r->value, sizeof(r->value), "%s", gov_label(st.cpu_gov));
            d = _("How the CPU frequency follows the load. Performance always keeps the maximum frequency.");
            break;
        case R_CPU_MAX:
            snprintf(r->label, sizeof(r->label), "%s", _("CPU maximum frequency"));
            snprintf(r->value, sizeof(r->value), "%d MHz", st.cpu_max_mhz);
            d = _("Upper frequency limit. Persists after restart.");
            if (plat->fake_oc_check && st.cpu_max_mhz > CPU_STOCK_MAX_MHZ && i == sel && k.a &&
                oc_is_real(st.cpu_max_mhz * 1000) == 0) r->value_col = C_WARN;
            break;
        case R_TURBO:
            snprintf(r->label, sizeof(r->label), "%s", _("Turbo"));
            snprintf(r->value, sizeof(r->value), "%s", st.boost == 1 ? _("On") : _("Off"));
            if (st.boost == 1) r->value_col = C_WARN;
            d = _("Enables 1416 and 1512 MHz at 1350 mV. Raises temperature and may cause instability. Persists after restart.");
            break;
        case R_GPU_MODE:
            snprintf(r->label, sizeof(r->label), "%s", _("GPU governor"));
            snprintf(r->value, sizeof(r->value), "%s", mode_name(mode_of(st.gpu_gov)));
            d = _("How the GPU frequency follows the load. On demand may stay too low for emulators.");
            break;
        case R_GPU_MAX:
            snprintf(r->label, sizeof(r->label), "%s", _("GPU maximum frequency"));
            snprintf(r->value, sizeof(r->value), "%d MHz", st.gpu_max_mhz);
            d = _("Upper frequency limit. Persists after restart.");
            break;
        case R_RAM_GOV:
            snprintf(r->label, sizeof(r->label), "%s", _("Memory governor"));
            snprintf(r->value, sizeof(r->value), "%s", mode_name(mode_of(st.ram_gov)));
            d = _("How the memory frequency follows the load. On demand may stay too low for emulators.");
            break;
        case R_RAM_MAX:
            snprintf(r->label, sizeof(r->label), "%s", _("Memory maximum frequency"));
            snprintf(r->value, sizeof(r->value), "%d MHz", st.ram_max_mhz);
            d = _("Upper frequency limit. Persists after restart.");
            break;
        case R_PERF: {
            char cur[16];
            plat->perfmode_get(cur, sizeof(cur));
            snprintf(r->label, sizeof(r->label), "%s", _("Performance mode in games"));
            snprintf(r->value, sizeof(r->value), "%s", !strcmp(cur, "1") ? _("On") : !strcmp(cur, "0") ? _("Off") : _("Auto"));
            d = _("Keeps GPU and memory at maximum frequency while a game runs. Largest gain in demanding games.");
            break;
        }
        case R_LANG:
            snprintf(r->label, sizeof(r->label), "%s", _("Language"));
            snprintf(r->value, sizeof(r->value), "%s", strcmp(i18n_get(), "es") ? "English" : "Español");
            d = _("Interface language.");
            break;
        }
        if (i == sel) desc = d;
        int th = ui_wrap_h(F_SM, d, w - 2 * UI_PAD) + G(3);
        if (th > dh) dh = th;
        /* value with arrows: everything here changes with left and right */
        char arrows[40];
        snprintf(arrows, sizeof(arrows), "‹ %.30s ›", r->value);
        snprintf(r->value, sizeof(r->value), "%s", arrows);
    }

    /* explanation of the selected row, always in the same place */
    ui_list(rows, n, sel, &first, top, bottom - dh - G(1));
    ui_round(UI_M, bottom - dh, w, dh, UI_RAD, C_PANEL);
    ui_wrap(F_SM, desc, UI_M + UI_PAD, bottom - dh + (dh - ui_wrap_h(F_SM, desc, w - 2 * UI_PAD)) / 2, w - 2 * UI_PAD, C_DIM);

    const char *hints[4][2] = {{"<>", _("Change")}, {"L R", _("Tab")}, {"B", _("Back")}, {NULL, NULL}};
    ui_hints(hints);
}
