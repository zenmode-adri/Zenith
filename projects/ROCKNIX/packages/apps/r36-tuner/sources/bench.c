/* SPDX-License-Identifier: MIT
 * The tests. Each one runs a small program in a thread while the screen updates twice a second, which keeps the tuner
 * itself out of the measurement. */
#include <pthread.h>
#include <signal.h>
#include "tuner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int t_start, t_max, t_sum, t_n;
} Temps;

static void temps_add(Temps *t) {
    int v = sys_int(CPU_TEMP) / 1000;
    if (v <= 0) return;
    if (!t->t_n) t->t_start = v;
    if (v > t->t_max) t->t_max = v;
    t->t_sum += v;
    t->t_n++;
}

static int temps_avg(const Temps *t) { return t->t_n ? t->t_sum / t->t_n : 0; }

static void scores_path(char *p, int n) { snprintf(p, n, "%s", rp(plat->data_dir)); strncat(p, "/scores.log", n - strlen(p) - 1); }

/* date | type | detail | temperatures. English and fixed: older results and the CPU overclock check read this format */
static void log_result(const char *type, const char *detail, const Temps *t) {
    char p[300], date[32];
    scores_path(p, sizeof(p));
    FILE *f = fopen(p, "a");
    if (!f) return;
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(f, "%s | %s | %s | %dC -> %dC -> %dC peak\n", date, type, detail, t->t_start, temps_avg(t), t->t_max);
    fclose(f);
}

/* First number of the last result of this type, to compare with */
static double last_score(const char *type) {
    char p[300], line[256], key[16];
    double v = -1;
    scores_path(p, sizeof(p));
    snprintf(key, sizeof(key), "| %s |", type);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char *a = strstr(line, key);
        if (!a) continue;
        a += strlen(key);
        while (*a && (*a < '0' || *a > '9')) a++;
        if (*a) v = atof(a);
    }
    fclose(f);
    return v;
}

int bench_passed_at(int mhz) {
    char p[300], b[16];
    scores_path(p, sizeof(p));
    sys_cap(b, sizeof(b), "grep -c '| CPU | .*@ %d MHz' '%s' 2>/dev/null", mhz, p);
    return atoi(b) > 0;
}

/* ── Running a program in the background ─────────────────────────────────── */
typedef struct {
    char cmd[1200];
    char out[4096];
    volatile int done;
    volatile int lines;
    void *f;
} Job;

static int job_thread(void *data) {
    Job *j = data;
    char line[300];
    size_t used = 0;
    /* SDL threads block SIGTERM, and a program started from here would inherit that: nothing could stop it, not the
       B button, not the temperature limit, not leaving the program */
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    j->f = sys_popen_root(j->cmd);
    if (j->f) {
        while (fgets(line, sizeof(line), (FILE *)j->f)) {
            size_t l = strlen(line);
            if (used + l >= sizeof(j->out)) used = 0;   /* only the end matters */
            memcpy(j->out + used, line, l + 1);
            used += l;
            if (strstr(line, "FPS:")) j->lines++;
        }
        sys_pclose(j->f);
    }
    j->done = 1;
    return 0;
}

/* Common waiting screen. Returns 0 finished, 1 cancelled. */
typedef enum { WATCH_CPU, WATCH_RAM, WATCH_GPU } Watch;

/* Temperature and the part under test, with their graphs */
static void watch_cards(int y, int bottom, Watch w, const Temps *t) {
    char v[32], s[40];
    int h = bottom - y;
    snprintf(v, sizeof(v), "%.0f °C", st.temp);
    snprintf(s, sizeof(s), "%s %d °C", _("Peak"), t->t_max);
    ui_card(ui_col(0), y, ui_span(0, 6), h, _("Temperature"), v, C_TXT, s, st.h_temp, 30, 90, st.temp >= 85 ? C_BAD : st.temp >= 75 ? C_WARN : C_OK, "°C", 85);
    int mhz = w == WATCH_CPU ? st.cpu_mhz : w == WATCH_RAM ? st.ram_mhz : st.gpu_mhz;
    int max = w == WATCH_CPU ? st.cpu_max_mhz : w == WATCH_RAM ? st.ram_max_mhz : st.gpu_max_mhz;
    const float *hist = w == WATCH_CPU ? st.h_cpu : w == WATCH_RAM ? st.h_ram : st.h_gpu;
    snprintf(v, sizeof(v), "%d MHz", mhz);
    snprintf(s, sizeof(s), "%s %d", _("max"), max);
    ui_card(ui_col(6), y, ui_span(6, 6), h, w == WATCH_CPU ? "CPU" : w == WATCH_RAM ? _("Memory") : "GPU", v, C_TXT, s, hist, 0, max > 0 ? (float)max : 1, C_ACCENT, "MHz", 0);
}

static int job_wait(Job *j, const char *title, int total_s, const char *kill_cmd, Temps *t, int scenes, Watch watch) {
    SDL_Thread *th = SDL_CreateThread(job_thread, "bench", j);
    if (!th) return 1;
    Uint32 start = SDL_GetTicks();
    Uint32 last_t = start - 2000, last_s = start;
    int cancelled = 0;
    while (!j->done && ui_running) {
        Uint32 now = SDL_GetTicks();
        if (now - last_t >= 2000) { temps_add(t); last_t = now; }
        if (now - last_s >= 1000) { state_refresh(); last_s = now; }
        int el = (int)((now - start) / 1000);
        ui_clear();
        ui_header(title, NULL);
        int x = UI_M, w = UW - 2 * x, y = ui_content_top(), bot = ui_content_bottom() - UI_GAP;
        char b[64];
        ui_text(F_MD, _("Running…"), x, y, C_TXT);
        if (total_s) snprintf(b, sizeof(b), "%d:%02d / %d:%02d", el / 60, el % 60, total_s / 60, total_s % 60);
        else snprintf(b, sizeof(b), "%d:%02d", el / 60, el % 60);
        ui_text_r(F_SM, b, x + w, y + (ui_fh(F_MD) - ui_fh(F_SM)) / 2, C_DIM);
        y += ui_fh(F_MD) + G(1);
        float fr = scenes ? (float)j->lines / (float)scenes : total_s ? (float)el / (float)total_s : 0;
        ui_bar(x, y, w, G(2), fr, C_ACCENT);
        y += G(2) + UI_GAP;
        watch_cards(y, bot, watch, t);
        const char *hints[2][2] = {{"B", _("Stop")}, {NULL, NULL}};
        ui_hints(hints);
        ui_present();
        Keys k = ui_poll(500);
        if (k.b && !cancelled) { cancelled = 1; sys_root("%s", kill_cmd); }
    }
    /* Closed from outside: without this the wait below lasts as long as the program, and what it was given stays until then */
    if (!j->done && !cancelled) { cancelled = 1; sys_root("%s", kill_cmd); }
    SDL_WaitThread(th, NULL);
    return cancelled;
}

static void compare_line(char *out, int n, double now, double before) {
    out[0] = 0;
    if (before <= 0) return;
    double d = (now - before) / before * 100.0;
    if (d > 1) snprintf(out, n, "%s (+%.0f %%)", _("Higher than the previous result"), d);
    else if (d < -1) snprintf(out, n, "%s (%.0f %%)", _("Lower than the previous result"), d);
    else snprintf(out, n, "%s", _("Same as the previous result"));
}

/* The last results of this type, oldest first; the first number of each line */
#define RESULTS_MAX 8
static int recent_results(const char *type, double *out) {
    char p[300], line[256], key[16];
    int n = 0;
    scores_path(p, sizeof(p));
    snprintf(key, sizeof(key), "| %s |", type);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *a = strstr(line, key);
        if (!a) continue;
        a += strlen(key);
        while (*a && (*a < '0' || *a > '9')) a++;
        if (!*a) continue;
        if (n == RESULTS_MAX) { memmove(out, out + 1, sizeof(double) * (RESULTS_MAX - 1)); n--; }
        out[n++] = atof(a);
    }
    fclose(f);
    return n;
}

static void results_chart(int x, int y, int w, int h, const double *v, int n) {
    ui_round(x, y, w, h, UI_RAD, C_PANEL);
    ui_text(F_SM, _("Recent results"), x + UI_PAD, y + UI_GAP, C_DIM);
    double max = 0;
    for (int i = 0; i < n; i++) if (v[i] > max) max = v[i];
    if (max <= 0) return;
    int fh = ui_fh(F_SM), pt = y + UI_GAP + fh + G(1) + fh, pb = y + h - UI_GAP, inner = w - 2 * UI_PAD;
    int slot = inner / RESULTS_MAX, bw = slot * 60 / 100;
    for (int i = 0; i < n; i++) {
        int cx = x + w / 2 + (2 * i - n + 1) * slot / 2, last = i == n - 1;
        int bh = (int)((pb - pt) * v[i] / max);
        if (bh < G(1)) bh = G(1);
        ui_round(cx - bw / 2, pb - bh, bw, bh, U(4), last ? C_ACCENT : C_PANEL2);
        char t[24];
        snprintf(t, sizeof(t), "%.0f", v[i]);
        ui_text_c(F_SM, t, cx, pb - bh - fh, last ? C_TXT : C_MUTED);
    }
}

/* Result screen: big number, comparison, temperatures; below them your last results when there are some to compare with.
   Without them the block is centred in the free space. 1 = run again */
static int result_page(const char *title, const char *type, const char *big, const char *unit, const char *big2, const char *unit2,
                       const char *cmp, const Temps *t) {
    int sel = 0;
    double hist[RESULTS_MAX];
    int nh = recent_results(type, hist);
    for (;;) {
        ui_clear();
        ui_header(title, _("Result"));
        int top = ui_content_top(), bot = ui_buttons_top() - UI_GAP;
        int has_cmp = cmp && cmp[0];
        int block = ui_fh(F_XL) + ui_fh(F_SM) + (has_cmp ? G(1) + ui_fh(F_MD) : 0) + G(1) + ui_fh(F_SM);
        int chart_h = bot - top - block - UI_GAP;
        int chart = nh >= 2 && chart_h >= G(20);
        int y = chart ? top : top + (bot - top - block) / 2;
        if (big2) {
            int c1 = ui_col(0) + ui_span(0, 6) / 2, c2 = ui_col(6) + ui_span(6, 6) / 2;
            ui_text_c(F_XL, big, c1, y, C_ACCENT);
            ui_text_c(F_XL, big2, c2, y, C_ACCENT);
            y += ui_fh(F_XL);
            ui_text_c(F_SM, unit, c1, y, C_DIM);
            ui_text_c(F_SM, unit2, c2, y, C_DIM);
        } else {
            ui_text_c(F_XL, big, UW / 2, y, C_ACCENT);
            y += ui_fh(F_XL);
            ui_text_c(F_SM, unit, UW / 2, y, C_DIM);
        }
        y += ui_fh(F_SM);
        if (has_cmp) { y += G(1); ui_text_c(F_MD, cmp, UW / 2, y, C_TXT); y += ui_fh(F_MD); }
        y += G(1);
        char b[64];
        snprintf(b, sizeof(b), "%s: %d - %d °C", _("Temperature"), t->t_start, t->t_max);
        ui_text_c(F_SM, b, UW / 2, y, C_DIM);
        y += ui_fh(F_SM);
        if (chart) results_chart(UI_M, y + UI_GAP, UW - 2 * UI_M, bot - y - UI_GAP, hist, nh);
        ui_buttons(_("Run again"), _("Back"), sel, 0);
        const char *hints[3][2] = {{"A", _("Select")}, {"B", _("Back")}, {NULL, NULL}};
        ui_hints(hints);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return 0;
        if (k.left || k.right) sel ^= 1;
        if (k.a) return sel == 0;
        if (k.b || k.sel) return 0;
    }
}

/* ── CPU ────────────────────────────────────────────────────────────────── */
void bench_cpu(void) {
    const char *title = _("CPU benchmark");
    const char *bin = bench_path("r36_cpubench");
    if (!sys_exists(bin) && access(bin, 1)) { page_info(title, _("Benchmark program not found. Reinstall R36 Tuner."), 0); return; }
    for (;;) {
        char gov[32];
        sys_read(CPU_POLICY "/scaling_governor", gov, sizeof(gov));
        sys_write(CPU_POLICY "/scaling_governor", "performance");
        int mhz = sys_int(CPU_POLICY "/scaling_max_freq") / 1000;
        Job j;
        memset(&j, 0, sizeof(j));
        snprintf(j.cmd, sizeof(j.cmd), "%s 2>/dev/null", bin);
        Temps t = {0};
        int c = job_wait(&j, title, 30, "kill $(pidof r36_cpubench) 2>/dev/null", &t, 0, WATCH_CPU);
        sys_write(CPU_POLICY "/scaling_governor", gov);
        if (c || !ui_running) return;
        long long mops = atoll(j.out) / 1000000;
        if (mops <= 0) { page_info(title, _("Benchmark program not found. Reinstall R36 Tuner."), 0); return; }
        double before = last_score("CPU");
        char big[32], det[80], cmp[100];
        snprintf(big, sizeof(big), "%lld", mops);
        snprintf(det, sizeof(det), "%lld Mops/30s @ %d MHz", mops, mhz);
        log_result("CPU", det, &t);
        compare_line(cmp, sizeof(cmp), (double)mops, before);
        char unit[48];
        snprintf(unit, sizeof(unit), "Mops/30s @ %d MHz", mhz);
        if (!result_page(title, "CPU", big, unit, NULL, NULL, cmp, &t)) return;
    }
}

/* ── Memory: pinned to its top speed, and reports the speed it really ran at ─ */
void bench_ram(void) {
    const char *title = _("Memory benchmark");
    const char *bin = bench_path("r36_rambench");
    if (!sys_exists(bin) && access(bin, 1)) { page_info(title, _("Benchmark program not found. Reinstall R36 Tuner."), 0); return; }
    for (;;) {
        char gov[32];
        sys_read(DMC_DEVFREQ "/governor", gov, sizeof(gov));
        plat->set_ram_gov("performance");
        SDL_Delay(100);
        int mhz = (int)(sys_ll(DMC_DEVFREQ "/cur_freq") / 1000000);
        Job j;
        memset(&j, 0, sizeof(j));
        snprintf(j.cmd, sizeof(j.cmd), "%s 2>/dev/null", bin);
        Temps t = {0};
        int c = job_wait(&j, title, 30, "kill $(pidof r36_rambench) 2>/dev/null", &t, 0, WATCH_RAM);
        sys_write(DMC_DEVFREQ "/governor", gov);
        if (c || !ui_running) return;
        long long wr = 0, cp = 0;
        sscanf(j.out, "%lld %lld", &wr, &cp);
        if (wr <= 0) { page_info(title, _("Benchmark program not found. Reinstall R36 Tuner."), 0); return; }
        double before = last_score("RAM");
        char a[24], b[24], det[96], cmp[100], u1[48], u2[48];
        snprintf(a, sizeof(a), "%lld", wr);
        snprintf(b, sizeof(b), "%lld", cp);
        snprintf(det, sizeof(det), "Write %lld MB/s | Copy %lld MB/s @ %d MHz", wr, cp, mhz);
        log_result("RAM", det, &t);
        compare_line(cmp, sizeof(cmp), (double)wr, before);
        snprintf(u1, sizeof(u1), "%s MB/s @ %d MHz", _("Write"), mhz);
        snprintf(u2, sizeof(u2), "%s MB/s", _("Copy"));
        if (!result_page(title, "RAM", a, u1, b, u2, cmp, &t)) return;
    }
}

/* ── GPU ────────────────────────────────────────────────────────────────── */
int bench_available_gpu(void) { return plat->gpu_bench_cmd && plat->gpu_bench_cmd(); }

void bench_gpu(void) {
    const char *title = _("GPU benchmark");
    for (;;) {
        long long max_hz = sys_ll(GPU_DEVFREQ "/max_freq"), min_hz = sys_ll(GPU_DEVFREQ "/min_freq");
        char v[32];
        snprintf(v, sizeof(v), "%lld", max_hz);
        sys_write(GPU_DEVFREQ "/min_freq", v);    /* keeps the governor from slowing it down */
        Job j;
        memset(&j, 0, sizeof(j));
        snprintf(j.cmd, sizeof(j.cmd), "%s 2>&1", plat->gpu_bench_cmd());
        Temps t = {0};
        int c = job_wait(&j, title, 0, "pkill -f glmark2-es2", &t, 4, WATCH_GPU);
        snprintf(v, sizeof(v), "%lld", min_hz);
        sys_write(GPU_DEVFREQ "/min_freq", v);
        if (c || !ui_running) return;
        char *s = strstr(j.out, "glmark2 Score:");
        if (!s) { page_info(title, _("The GPU benchmark failed."), 0); return; }
        int score = atoi(s + 14);
        double before = last_score("GPU");
        char big[24], det[64], cmp[100], unit[48];
        snprintf(big, sizeof(big), "%d", score);
        snprintf(det, sizeof(det), "%d pts @ %lldMHz", score, max_hz / 1000000);
        log_result("GPU", det, &t);
        compare_line(cmp, sizeof(cmp), (double)score, before);
        snprintf(unit, sizeof(unit), "%s @ %lld MHz", _("points"), max_hz / 1000000);
        if (!result_page(title, "GPU", big, unit, NULL, NULL, cmp, &t)) return;
    }
}

static void show_detail(char *out, int n, const char *logged);
#define HIST_MAX 200
typedef struct { char date[24], type[8], detail[96], temp[48]; } Entry;
static int history_load(Entry *e);

/* "Last result: ..." if the test has run before */
static int last_stability(char *out, int n) {
    static Entry e[HIST_MAX];
    int nh = history_load(e);
    for (int i = 0; i < nh; i++)
        if (!strcmp(e[i].type, "STB")) {
            char d[140];
            show_detail(d, sizeof(d), e[i].detail);
            snprintf(out, n, "%s: %s (%s)", _("Last result"), d, e[i].date);
            return 1;
        }
    return 0;
}

/* ── Stability: everything loaded, errors counted, and a crash undoes the tuning ─ */
#define STAB_HOT 88
/* not pkill -x: on a real Zenith console it finds nothing and the test keeps running */
#define STOP_STRESS "kill $(pidof r36_stress) 2>/dev/null"

/* A change counts as tested only if it ran: every step whose voltage differs from factory takes its turn at the top, under load */
#define STEP_SECS 15
#define STEP_SEEN_SECS 5
typedef struct {
    const char *name, *table, *max_file, *cur_file, *avail_file;
    long long hz_per_unit;
    char limit[32];          /* what max_file had before the test */
    long long hz[MAX_OPP];   /* as the kernel lists them; memory answers near that, not exactly */
    int seen[MAX_OPP];       /* seconds under load on it */
    int n, at;
} Part;

/* A step that this kernel does not offer now (Turbo off) cannot be reached and is not running either */
static void part_steps(Part *p, const char *factory, int ram) {
    Opp now[MAX_OPP], fac[MAX_OPP];
    char avail[700], *tok[32];
    if (ram) dtb_use_ram_bin();
    int nn = dtb_scan(p->table, now, MAX_OPP);
    int nf = dtb_scan_factory(factory, p->table, fac, MAX_OPP);
    if (ram) dtb_use_bin(NULL);
    if (sys_read(p->avail_file, avail, sizeof(avail))) return;
    if (p->hz_per_unit == 1000 && sys_int(BOOST_FILE) == 1) {
        char more[200];
        if (!sys_read(CPU_POLICY "/scaling_boost_frequencies", more, sizeof(more)))
            snprintf(avail + strlen(avail), sizeof(avail) - strlen(avail), " %s", more);
    }
    int na = sys_split(avail, tok, 32);
    for (int i = 0; i < nn && p->n < MAX_OPP; i++) {
        int changed = 1;
        for (int f = 0; f < nf; f++) if (fac[f].hz == now[i].hz) changed = fac[f].uv != now[i].uv;
        if (!changed) continue;
        for (int k = 0; k < na; k++) {
            long long hz = atoll(tok[k]) * p->hz_per_unit;
            if (llabs(hz - now[i].hz) * 50 <= now[i].hz) { p->hz[p->n++] = hz; break; }
        }
    }
}

static void part_go(Part *p) {
    char v[32];
    if (!p->n) return;
    snprintf(v, sizeof(v), "%lld", p->hz[p->at] / p->hz_per_unit);
    sys_write(p->max_file, v);
}

static void part_sample(Part *p) {
    long long cur = sys_ll(p->cur_file) * p->hz_per_unit;
    for (int i = 0; i < p->n; i++) if (llabs(cur - p->hz[i]) * 50 <= p->hz[i]) p->seen[i]++;
}

/* "S <seconds under load> <errors> <1 cooling>": the last one in what the program has printed */
static int stress_status(const char *out, int *active, int *errs, int *cooling) {
    const char *last = NULL;
    for (const char *p = out; (p = strstr(p, "S ")); p++) if (p == out || p[-1] == '\n') last = p;
    return last && sscanf(last, "S %d %d %d", active, errs, cooling) == 3;
}

void bench_stability(void) {
    const char *title = _("Stability test");
    const char *bin = bench_path("r36_stress");
    if (!sys_exists(bin) && access(bin, 1)) { page_info(title, _("Benchmark program not found. Reinstall R36 Tuner."), 0); return; }
    Row m[3];
    memset(m, 0, sizeof(m));
    snprintf(m[0].label, sizeof(m[0].label), "%s", _("Quick: 5 minutes"));
    snprintf(m[0].desc, sizeof(m[0].desc), "%s", _("After minor changes."));
    snprintf(m[1].label, sizeof(m[1].label), "%s", _("Normal: 20 minutes"));
    snprintf(m[1].desc, sizeof(m[1].desc), "%s", _("Recommended."));
    snprintf(m[2].label, sizeof(m[2].label), "%s", _("Extended: 60 minutes"));
    snprintf(m[2].desc, sizeof(m[2].desc), "%s", _("To validate a final configuration."));
    const char *info[5];
    int ni = 0;
    char last[120];
    info[ni++] = _("CPU: all cores repeat calculations with a known answer.");
    info[ni++] = _("Memory: writes patterns and reads them back.");
    if (bench_available_gpu()) info[ni++] = _("GPU: runs a graphics load at the same time.");
    info[ni++] = _("If it gets too hot it waits for the chip to cool down, so it can take longer than the time chosen.");
    if (last_stability(last, sizeof(last))) info[ni++] = last;
    int c = page_list_info(title, _("Duration"), m, 3, 1, _("What it checks"), info, ni);
    if (c < 0) return;
    static const int secs[3] = {300, 1200, 3600};
    int total = secs[c];

    app_status_refresh();
    /* Installed but not booted yet: the test would run on the settings of this boot and say nothing about the new ones */
    if (app_dtb.reboot_needed) {
        page_info(title, _("Your changes apply after restarting. Restart the console, then run this test."), 0);
        return;
    }
    int tuned = app_dtb.tuned;
    const int was_ok = tuned && app_dtb.net == NET_OK;   /* a test without a result does not take that away */
    if (tuned) {
        const char *warn[1] = {_("If the console hangs or restarts during this test, your changes are reverted on the next startup, as long as that boot reaches the operating system.")};
        if (!page_confirm(title, NULL, warn, 1, _("Start"), _("Cancel"), 0)) return;
        if (plat->dtb_trial(1)) {
            page_info(title, _("The safety net could not be turned on. The test was not started."), 0);
            return;
        }
    }

    /* Everything at the top, so the test reaches the speeds that matter */
    Part parts[3] = {
        {.name = "CPU", .table = dtb_cpu_table(), .max_file = CPU_POLICY "/scaling_max_freq", .cur_file = CPU_POLICY "/scaling_cur_freq",
         .avail_file = CPU_POLICY "/scaling_available_frequencies", .hz_per_unit = 1000},
        {.name = "GPU", .table = dtb_gpu_table(), .max_file = GPU_DEVFREQ "/max_freq", .cur_file = GPU_DEVFREQ "/cur_freq",
         .avail_file = GPU_DEVFREQ "/available_frequencies", .hz_per_unit = 1},
        {.name = "RAM", .table = dtb_ram_table(), .max_file = DMC_DEVFREQ "/max_freq", .cur_file = DMC_DEVFREQ "/cur_freq",
         .avail_file = DMC_DEVFREQ "/available_frequencies", .hz_per_unit = 1},
    };
    char factory[300] = "";
    /* Without changes to test the limits the user set stay */
    if (tuned && !plat->dtb_factory(factory, sizeof(factory)) && factory[0])
        for (int i = 0; i < 3; i++) {
            sys_read(parts[i].max_file, parts[i].limit, sizeof(parts[i].limit));
            part_steps(&parts[i], factory, i == 2);
        }
    const char *step_env = getenv("R36T_STEP_SECS");   /* tests: seconds, not minutes */
    const int step_secs = step_env ? atoi(step_env) : STEP_SECS, seen_secs = step_env ? 1 : STEP_SEEN_SECS;

    char cgov[32], ggov[32], rgov[32];
    sys_read(CPU_POLICY "/scaling_governor", cgov, sizeof(cgov));
    sys_read(GPU_DEVFREQ "/governor", ggov, sizeof(ggov));
    sys_read(DMC_DEVFREQ "/governor", rgov, sizeof(rgov));
    sys_write(CPU_POLICY "/scaling_governor", "performance");
    /* Written directly: a setting would stay after the test, and a console frozen by the test would boot with it */
    if (plat->hold_gpu) plat->hold_gpu(1);
    sys_write(GPU_DEVFREQ "/governor", "performance");
    plat->set_ram_gov("performance");
    for (int i = 0; i < 3; i++) part_go(&parts[i]);

    Job j;
    memset(&j, 0, sizeof(j));
    const char *gpu = plat->gpu_bench_cmd ? plat->gpu_bench_cmd() : NULL;
    /* the limit is in the program: it holds even if this one dies */
    if (gpu)
        snprintf(j.cmd, sizeof(j.cmd), "%s %d --hot %d --temp '%s' --gpu '%s'", bin, total, STAB_HOT, rp(CPU_TEMP), gpu);
    else
        snprintf(j.cmd, sizeof(j.cmd), "%s %d --hot %d --temp '%s'", bin, total, STAB_HOT, rp(CPU_TEMP));

    SDL_Thread *th = SDL_CreateThread(job_thread, "stress", &j);
    Temps t = {0};
    Uint32 start = SDL_GetTicks(), last_t = start - 2000, last_s = start;
    int stopped = 0, under_load = 0;
    while (th && !j.done && ui_running) {
        Uint32 now = SDL_GetTicks();
        int el = (int)((now - start) / 1000), errs = 0, cooling = 0;
        if (!stress_status(j.out, &el, &errs, &cooling)) { errs = 0; cooling = 0; }
        if (now - last_t >= 2000) { temps_add(&t); last_t = now; }
        if (now - last_s >= 1000) {
            state_refresh();
            last_s = now;
            if (!cooling) {
                under_load++;
                for (int i = 0; i < 3; i++) {
                    if (i != 1 || gpu) part_sample(&parts[i]);   /* the GPU at a speed with nothing to draw proves nothing */
                    if (parts[i].n > 1 && under_load % step_secs == 0) {
                        parts[i].at = (parts[i].at + 1) % parts[i].n;
                        part_go(&parts[i]);
                    }
                }
            }
        }
        int left = total - el < 0 ? 0 : total - el;
        char b[80];

        ui_clear();
        ui_header(title, NULL);
        int x = UI_M, w = UW - 2 * x, y = ui_content_top(), bot = ui_content_bottom() - UI_GAP;
        ui_text(F_MD, cooling ? _("Cooling down") : _("Time remaining"), x, y, C_TXT);
        snprintf(b, sizeof(b), "%d:%02d", left / 60, left % 60);
        ui_text_r(F_MD, b, x + w, y, C_TXT);
        y += ui_fh(F_MD) + G(1);
        ui_bar(x, y, w, G(2), (float)el / (float)total, C_ACCENT);
        y += G(2) + UI_GAP;
        int avail = bot - y, row2 = (avail - UI_GAP) / 2, row1 = avail - UI_GAP - row2;
        char s[40];
        snprintf(b, sizeof(b), "%.0f °C", st.temp);
        snprintf(s, sizeof(s), "%s %d °C", _("Peak"), t.t_max);
        ui_card(ui_col(0), y, ui_span(0, 6), row1, _("Temperature"), b, C_TXT, s, st.h_temp, 30, 90, st.temp >= 85 ? C_BAD : st.temp >= 75 ? C_WARN : C_OK, "°C", 85);
        /* the number that matters in this test: large, in the middle of its card */
        {
            int ex = ui_col(6), ew = ui_span(6, 6), blk = ui_fh(F_XL) + ui_fh(F_SM);
            ui_round(ex, y, ew, row1, UI_RAD, C_PANEL);
            ui_text(F_SM, _("Errors"), ex + UI_PAD, y + UI_GAP, C_DIM);
            int ey = y + UI_GAP + ui_fh(F_SM) + (row1 - UI_GAP - ui_fh(F_SM) - blk) / 2;
            snprintf(b, sizeof(b), "%d", errs);
            ui_text_c(F_XL, b, ex + ew / 2, ey, errs ? C_BAD : C_OK);
            ui_text_c(F_SM, errs ? _("This configuration is not stable.") : _("None so far."), ex + ew / 2, ey + ui_fh(F_XL), C_DIM);
        }
        y += row1 + UI_GAP;
        const struct { const char *t; int cur, max; const float *h; } sp[3] = {
            {"CPU", st.cpu_mhz, st.cpu_max_mhz, st.h_cpu},
            {"GPU", st.gpu_mhz, st.gpu_max_mhz, st.h_gpu},
            {_("Memory"), st.ram_mhz, st.ram_max_mhz, st.h_ram},
        };
        for (int i = 0; i < 3; i++) {
            snprintf(b, sizeof(b), "%d MHz", sp[i].cur);
            snprintf(s, sizeof(s), "%s %d", _("max"), sp[i].max);
            ui_card(ui_col(i * 4), y, ui_span(i * 4, 4), row2, sp[i].t, b, C_TXT, s, sp[i].h, 0, sp[i].max > 0 ? (float)sp[i].max : 1, C_ACCENT, "MHz", 0);
        }
        const char *hints[2][2] = {{"B", _("Stop")}, {NULL, NULL}};
        ui_hints(hints);
        ui_present();
        Keys k = ui_poll(500);
        if (k.b && !stopped) { stopped = 1; sys_root("%s", STOP_STRESS); }
    }
    /* Closed from outside: no reason to wait for the whole test */
    if (th && !j.done && !stopped) { stopped = 1; sys_root("%s", STOP_STRESS); }
    if (th) SDL_WaitThread(th, NULL);

    for (int i = 0; i < 3; i++) if (parts[i].limit[0]) sys_write(parts[i].max_file, parts[i].limit);
    sys_write(CPU_POLICY "/scaling_governor", cgov);
    if (plat->hold_gpu) plat->hold_gpu(0);   /* first: with the mark still there, a change of battery state would put it back */
    sys_write(GPU_DEVFREQ "/governor", ggov);
    sys_write(DMC_DEVFREQ "/governor", rgov);

    int errs = -1, hot = 0;
    char *e = strstr(j.out, "RESULT ");
    if (e) { errs = atoi(e + 7); hot = strstr(e, " hot") != NULL; }
    if (tuned) plat->dtb_trial(!stopped && !hot && errs > 0 ? 2 : 0);
    int mins = (int)((SDL_GetTicks() - start) / 60000);
    char det[140], missed[100] = "";
    int nm = 0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < parts[i].n; k++)
            if (parts[i].seen[k] < seen_secs && nm++ < 4)
                snprintf(missed + strlen(missed), sizeof(missed) - strlen(missed), "%s%s %lld MHz", nm > 1 ? ", " : "", parts[i].name, parts[i].hz[k] / 1000000);
    if (nm > 4) snprintf(missed + strlen(missed), sizeof(missed) - strlen(missed), "…");
    if (hot) {
        snprintf(det, sizeof(det), "Stopped: too hot after %d min", mins);
        log_result("STB", det, &t);
        page_info(title, tuned && !was_ok ? _("Test stopped: temperature too high. Your changes remain unvalidated.") : _("Test stopped: temperature too high."), 0);
    } else if (stopped) {
        page_info(title, was_ok || !tuned ? _("Test stopped.") : _("Test stopped. Your changes remain unvalidated."), 0);
    } else if (errs == 0 && nm) {
        char msg[300];
        snprintf(det, sizeof(det), "Not validated: %s", missed);
        log_result("STB", det, &t);
        snprintf(msg, sizeof(msg), was_ok ? _("No errors, but these frequencies were not tested: %s. Your changes stay validated.")
                                          : _("No errors, but these frequencies were not tested: %s. Your changes remain unvalidated."), missed);
        page_info(title, msg, 0);
    } else if (errs == 0) {
        snprintf(det, sizeof(det), "Passed %d min", mins);
        log_result("STB", det, &t);
        if (tuned) {
            int unsaved = plat->dtb_confirm();
            app_status_refresh();
            if (unsaved) {
                page_info(_("Test passed"), _("No errors detected, but the result could not be saved. Your changes remain unvalidated."), 0);
                return;
            }
        }
        page_info(_("Test passed"), tuned ? _("No errors detected. Your changes are validated.") : _("No errors detected."), 0);
    } else {
        snprintf(det, sizeof(det), "Failed: %d errors in %d min", errs < 0 ? 0 : errs, mins);
        log_result("STB", det, &t);
        page_info(_("Test failed"), _("Errors detected: this configuration is not stable. Raise the voltage or restore factory settings."), 0);
    }
}

/* ── History ────────────────────────────────────────────────────────────── */
static void trim(char *s) {
    size_t n = strlen(s);
    while (n && strchr(" \t\r\n", s[n - 1])) s[--n] = 0;
    char *p = s;
    while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

/* Keeps the newest HIST_MAX lines of the file; shown newest first */
static int history_load(Entry *e) {
    char p[300], line[256];
    scores_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *a = strchr(line, '|');
        if (!a) continue;
        char *b = strchr(a + 1, '|');
        char *c = strrchr(line, '|');
        if (!b || c == a) continue;
        *a = 0; *c = 0;
        if (b != c) *b = 0;
        if (n == HIST_MAX) { memmove(e, e + 1, sizeof(Entry) * (HIST_MAX - 1)); n--; }
        snprintf(e[n].date, sizeof(e[n].date), "%s", line);
        snprintf(e[n].type, sizeof(e[n].type), "%s", a + 1);
        snprintf(e[n].detail, sizeof(e[n].detail), "%s", b != c ? b + 1 : "");
        snprintf(e[n].temp, sizeof(e[n].temp), "%s", c + 1);
        trim(e[n].date); trim(e[n].type); trim(e[n].detail); trim(e[n].temp);
        n++;
    }
    fclose(f);
    for (int i = 0, j = n - 1; i < j; i++, j--) { Entry x = e[i]; e[i] = e[j]; e[j] = x; }
    return n;
}

/* "40C -> 41C -> 42C peak" -> "40 - 42 °C" */
static void show_temp(char *out, int n, const char *logged) {
    int a, b, c;
    if (sscanf(logged, "%dC -> %dC -> %dC", &a, &b, &c) == 3) snprintf(out, n, "%d - %d °C", a, c);
    else snprintf(out, n, "%s", logged);
}

/* Words of the log, translated for the screen */
static void show_detail(char *out, int n, const char *logged) {
    static const char *const words[] = {"Write", "Copy", "Passed", "Not validated", "Failed:", "errors in", "Stopped: too hot after", NULL};
    char tmp[160];
    snprintf(out, n, "%s", logged);
    for (int i = 0; words[i]; i++) {
        char *p = strstr(out, words[i]);
        if (!p) continue;
        *p = 0;
        snprintf(tmp, sizeof(tmp), "%s%s%s", out, _(words[i]), p + strlen(words[i]));
        snprintf(out, n, "%s", tmp);
    }
}

void bench_history(void) {
    static Entry e[HIST_MAX];
    int n = history_load(e), first = 0;
    const char *title = _("History");
    if (!n) { page_info(title, _("No results yet."), 0); return; }
    for (;;) {
        ui_clear();
        ui_header(title, NULL);
        int top = ui_content_top(), bot = ui_content_bottom() - UI_GAP, rh = G(14), gap = G(1);
        int visible = (bot - top + gap) / (rh + gap);
        if (visible < 1) visible = 1;
        if (first > n - visible) first = n - visible > 0 ? n - visible : 0;
        for (int i = first; i < n && i < first + visible; i++) {
            int y = top + (i - first) * (rh + gap), x = UI_M, w = UW - 2 * UI_M;
            ui_round(x, y, w, rh, UI_RAD, C_PANEL);
            Col bc = !strcmp(e[i].type, "CPU") ? C_ACCENT : !strcmp(e[i].type, "GPU") ? C_WARN : !strcmp(e[i].type, "STB") ? C_OK : C_DIM;
            int bw = ui_tw(F_SM, e[i].type) + UI_PAD, bh = ui_fh(F_SM) + U(2);
            int ty = y + (rh - bh - G(1) - ui_fh(F_MD)) / 2;
            ui_round(x + UI_PAD, ty, bw, bh, U(5), bc);
            ui_text_c(F_SM, e[i].type, x + UI_PAD + bw / 2, ty + U(1), C_ON_ACCENT);
            ui_text(F_SM, e[i].date, x + UI_PAD + bw + UI_GAP, ty + U(1), C_DIM);
            char b[96], t[140];
            show_temp(t, sizeof(t), e[i].temp);
            ui_fit(b, sizeof(b), F_SM, t, w / 2 - UI_PAD);
            ui_text_r(F_SM, b, x + w - UI_PAD, ty + U(1), C_MUTED);
            show_detail(t, sizeof(t), e[i].detail);
            ui_fit(b, sizeof(b), F_MD, t, w - 2 * UI_PAD);
            ui_text(F_MD, b, x + UI_PAD, ty + bh + G(1), C_TXT);
        }
        ui_scrollbar(0, top, (visible < n ? visible : n) * (rh + gap) - gap, n, visible, first);
        const char *hints[3][2] = {{"X", _("Clear")}, {"B", _("Back")}, {NULL, NULL}};
        ui_hints(hints);
        ui_present();
        Keys k = ui_poll(1000);
        if (!ui_running) return;
        if (k.up && first > 0) first--;
        if (k.down && first < n - visible) first++;
        if (k.b || k.sel) return;
        if (k.x && page_confirm(title, _("Clear all results?"), NULL, 0, _("Clear"), _("Cancel"), 1)) {
            char p[300];
            scores_path(p, sizeof(p));
            remove(p);
            return;
        }
    }
}
