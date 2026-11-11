/* SPDX-License-Identifier: MIT
 * R36 Tuner Next: declarations shared by every module.
 *
 *   sys.c      files, sysfs and shell commands (every path goes through rp(), so tests can run on a fake root)
 *   plat.c     what differs between systems (Zenith, dArkOSRE): one binary, detected at start
 *   state.c    live readings (clocks, governors, temperature, battery) with a short history
 *   dtb.c      device tree edits, always on a temporary copy that the platform installs at the end
 *   i18n.c     translations: _("English text") -> current language
 *   ui.c       window, input, fonts and widgets
 *   tab_*.c    the four tabs; bench.c runs the tests
 */
#ifndef TUNER_H
#define TUNER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stddef.h>

#define CPU_POLICY  "/sys/devices/system/cpu/cpufreq/policy0"
#define BOOST_FILE  "/sys/devices/system/cpu/cpufreq/boost"
#define GPU_DEVFREQ "/sys/class/devfreq/ff400000.gpu"
#define DMC_DEVFREQ "/sys/class/devfreq/dmc"
#define CPU_TEMP    "/sys/class/thermal/thermal_zone0/temp"

#define OPP_FLOOR_UV 950000
#define UV_STEP_UV   12500
#define CPU_MAX_UV   1350000   /* vdd_arm ceiling */
#define LOGIC_MAX_UV 1150000   /* vdd_logic ceiling (GPU + RAM share it) */
#define CPU_STOCK_MAX_MHZ 1296

/* ── sys.c ─────────────────────────────────────────────────────────────── */
const char *rp(const char *path);                       /* real path: fake root prefix in tests */
int  sys_read(const char *path, char *buf, int n);      /* trimmed text; 0 ok, -1 missing */
int  sys_int(const char *path);
long long sys_ll(const char *path);
int  sys_exists(const char *path);
int  sys_write(const char *path, const char *val);      /* direct, or as root; 0 ok */
int  sys_root(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* shell as root, exit code */
int  sys_cap(char *out, int n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));   /* shell, captures stdout */
void *sys_popen_root(const char *cmd);                  /* FILE*, stdout of a shell as root; close with sys_pclose */
int  sys_pclose(void *f);
int  sys_split(char *text, char *tok[], int max);       /* splits on blanks, in place */
int  sys_has_word(const char *list, const char *word);
void sys_mkdirs(const char *path);

/* ── plat.c ────────────────────────────────────────────────────────────── */
typedef struct {
    int node_mhz;     /* name of the OPP node (opp-<node_mhz>000000) */
    int shown_mhz;    /* what the memory really reaches */
    int need_mhz;     /* node that has to exist first, 0 if none */
    int def_uv;       /* suggested voltage */
    int experimental;
} RamStep;

typedef enum { NET_NONE, NET_PENDING, NET_TRIAL, NET_OK, NET_UNDONE, NET_STALE } NetState;

typedef struct {
    NetState net;
    int tuned;            /* a modified device tree is in use or ready for the next boot */
    int overclocked;      /* boot.ini differs from its rescue copy */
    int reboot_needed;
} DtbStatus;

typedef struct Platform {
    const char *id;       /* "zenith", "darkosre" */
    const char *name;
    const char *data_dir;
    const char *const *fonts;       /* candidates, regular weight (first that exists wins) */
    int has_boost;        /* turbo = cpufreq/boost */
    int has_bootini_oc;   /* teacupx kernel: CPU OC through boot.ini */
    int has_lang_menu;    /* no system language to follow */
    int has_perfmode;     /* per game performance mode in the launcher */
    int joint_ram_gov;    /* the system sets memory governor together with the CPU one */
    int fake_oc_check;    /* kernels where a clock above 1296 MHz can be fake */
    int dmc_bins;         /* memory table has opp-microvolt-L<n> */
    int single_table;     /* the tuned device tree keeps one voltage column for CPU and GPU (see dtb_flatten) */
    const RamStep *ram_steps;
    int n_ram_steps;
    const RamStep *gpu_steps;     /* optional GPU frequencies */
    int n_gpu_steps;

    int  (*dtb_source)(char *out, int n);          /* device tree to start editing from; read only */
    int  (*dtb_factory)(char *out, int n);         /* the untouched device tree */
    int  (*dtb_install)(const char *edited);       /* installs the edited copy and arms the boot safety net */
    int  (*dtb_revert)(void);
    void (*dtb_status)(DtbStatus *st);
    int  (*dtb_trial)(int testing);                /* stress test: a crash while 1 undoes the tuning at next boot; 0 ends it, 2 ends it with errors */
    int  (*dtb_confirm)(void);                     /* stress test passed: the tuning is good */
    void (*dtb_ack)(void);                         /* the user has seen the "undone" notice */
    const char *recovery[8];                       /* steps shown in Recovery, English keys */

    void (*set_cpu_gov)(const char *gov);
    void (*set_gpu_mode)(const char *mode);
    void (*set_ram_gov)(const char *gov);
    void (*perfmode_get)(char *out, int n);
    void (*perfmode_set)(const char *val);
    void (*save_limits)(void);                     /* clock limits survive a reboot */
    void (*hold_gpu)(int on);                      /* while on, the system does not put the GPU governor back; NULL: nothing does */
    const char *(*language)(void);                 /* "es", "en", or NULL if unknown */
    void (*reboot)(void);
    const char *(*gpu_bench_cmd)(void);            /* NULL if there is no benchmark */
    const char *bench_dir;
} Platform;

extern const Platform *plat;
void plat_init(void);
const char *bench_path(const char *name);
void limits_save(void);

/* Profiles: what the one-button choice on the first tab does */
typedef enum { PROFILE_SAVE, PROFILE_BALANCED, PROFILE_PERF, PROFILE_CUSTOM } Profile;
Profile profile_current(void);
void profile_apply(Profile p);

/* ── state.c ───────────────────────────────────────────────────────────── */
#define HIST_N 60
typedef struct {
    int cpu_mhz, cpu_max_mhz, gpu_mhz, gpu_max_mhz, ram_mhz, ram_max_mhz;
    float temp;
    char cpu_gov[32], gpu_gov[32], ram_gov[32];
    char gpu_driver[24];
    int boost;            /* -1 no boost file */
    int bat_pct;          /* -1 no battery */
    int bat_charging;
    float bat_w;          /* < 0 unknown */
    float h_temp[HIST_N];
    float h_cpu[HIST_N], h_gpu[HIST_N], h_ram[HIST_N];
    float h_bat[HIST_N];
    int h_n;
} State;
extern State st;
void state_refresh(void);

/* ── i18n.c ────────────────────────────────────────────────────────────── */
const char *_(const char *en);
void i18n_init(void);
void i18n_set(const char *lang);
const char *i18n_get(void);

/* ── ui.c ──────────────────────────────────────────────────────────────── */
typedef struct { Uint8 r, g, b; } Col;
extern const Col C_BG, C_PANEL, C_PANEL2, C_LINE, C_TXT, C_DIM, C_MUTED, C_ACCENT, C_ON_ACCENT, C_OK, C_WARN, C_BAD;

typedef struct { int up, down, left, right, a, b, x, y, l, r, start, sel; } Keys;
typedef enum { F_SM, F_MD, F_LG, F_XL, F_COUNT } FontId;

extern volatile int ui_running;
extern int UW, UH;                 /* window size in pixels */

int  ui_init(int argc, char **argv);
void ui_quit(void);
int  U(int v);                     /* design pixels (640x480) -> real pixels */

/* The grid, in design pixels: everything is a multiple of 4. Page margin 16, 12 columns with gutter 8, panels with padding 12.
   Lists and tables start at the top of the content; messages and results are centred in the free space. */
#define G(n)    U((n) * 4)
#define UI_M    G(4)
#define UI_GAP  G(2)
#define UI_PAD  G(3)
#define UI_RAD  U(10)
int  ui_col(int i);                /* left edge of column i (0..12) */
int  ui_span(int i, int n);        /* width of n columns from column i */
int  ui_buttons_top(void);
void ui_buttons(const char *yes, const char *no, int sel, int danger);
int  ui_wrap_c(FontId f, const char *s, int y, int w, Col c);   /* centred lines; returns y after the text */
Keys ui_poll(int timeout_ms);      /* next key press or auto repeat, or nothing after timeout_ms */
void ui_clear(void);
void ui_present(void);
void ui_rect(int x, int y, int w, int h, Col c);
void ui_round(int x, int y, int w, int h, int rad, Col c);
void ui_line(int x1, int y1, int x2, int y2, Col c);
void ui_smooth_line(float x1, float y1, float x2, float y2, float thick, Col c, int alpha);
void ui_area(float x1, float y1, float x2, float y2, float base, Col c, int a_top, int a_base);
void ui_rect_a(int x, int y, int w, int h, Col c, int alpha);
void ui_card(int x, int y, int w, int h, const char *title, const char *value, Col value_col, const char *sub,
             const float *hist, float lo, float hi, Col c, const char *unit, float limit);
void ui_tag(int cx, int bottom, const char *txt);    /* small light label; stays inside the margins */
int  ui_text(FontId f, const char *s, int x, int y, Col c);
int  ui_text_r(FontId f, const char *s, int rx, int y, Col c);
int  ui_text_c(FontId f, const char *s, int cx, int y, Col c);
int  ui_wrap(FontId f, const char *s, int x, int y, int w, Col c);   /* returns y after the text */
int  ui_wrap_h(FontId f, const char *s, int w);
void ui_fit(char *out, int n, FontId f, const char *s, int w);       /* cuts with an ellipsis */
int  ui_tw(FontId f, const char *s);
int  ui_fh(FontId f);

/* Widgets */
typedef struct Row Row;
void ui_list(const Row *rows, int n, int sel, int *first, int top, int bot);
void ui_header(const char *title, const char *right);
void ui_hints(const char *const hints[][2]);       /* {"A","Select"},...,{NULL,NULL} */
void ui_bar(int x, int y, int w, int h, float frac, Col c);
int  ui_banner(int x, int y, int w, Col c, const char *msg);          /* wraps the text; returns its height */
int  ui_banner_h(const char *msg, int w);
void ui_scroll_mark(int on);       /* the scrollbar lives in the margin */
void ui_check_fits(const char *where, int bottom_of_content, int limit);  /* tests: reports what does not fit */
void ui_scrollbar(int x, int y, int h, int total, int visible, int first);
int  ui_content_top(void);
int  ui_content_bottom(void);

/* Pages: each one blocks until the user leaves it */
struct Row { char label[72]; char value[40]; char desc[140]; Col value_col; int disabled; };
int  page_list(const char *title, const char *sub, Row *rows, int n, int sel);   /* index or -1 */
int  page_list_info(const char *title, const char *sub, Row *rows, int n, int sel,
                    const char *info_title, const char *const *info, int n_info);  /* with a panel below */
void page_table_extra(void (*fn)(int x, int y, int w, int h, void *ctx), void *ctx);  /* draws in the free space of the next page_table */
int  page_confirm(const char *title, const char *msg, const char *const *warn, int n_warn,
                  const char *yes, const char *no, int danger);                  /* 1 yes */
void page_info(const char *title, const char *msg, int ms);                      /* ms 0: waits for a key */
void page_busy(const char *title, const char *msg);
void page_steps(const char *title, const char *const *steps, int n);             /* numbered cards */
typedef struct { char c[3][48]; Col col; } TableRow;
int  page_table(const char *title, const char *msg, const char *const hdr[3], const TableRow *rows, int n,
                const char *const *warn, int n_warn, const char *yes, const char *no);   /* 1 yes; yes NULL: only look */

/* ── dtb.c ─────────────────────────────────────────────────────────────── */
#define MAX_OPP 20
typedef struct { char node[160]; long long hz; int uv; } Opp;
int  dtb_begin(void);                              /* copies the source to a temporary file; 0 ok */
const char *dtb_tmp(void);
#define DTB_NOT_FROM_IMAGE 3   /* dtb_commit: the device tree in use is not one of the image's (r36-dtb install) */
int  dtb_commit(void);                             /* hands the copy to the platform; 0 ok */
int  dtb_went_factory(void);                       /* the last commit found factory values and restored factory instead */
void dtb_abort(void);
const char *dtb_bin(void);                         /* "opp-microvolt-L2", or "opp-microvolt" if unknown */
void dtb_use_bin(const char *prop);                /* the memory table may have its own column; NULL: back to the chip's */
const char *dtb_ram_bin(void);
void dtb_use_ram_bin(void);                        /* the column the kernel reads for the memory table; dtb_use_bin(NULL) goes back */
const char *dtb_cpu_table(void);
const char *dtb_gpu_table(void);
const char *dtb_ram_table(void);
void dtb_read_from(const char *file);              /* scans read this file; NULL: back to normal */
int  dtb_scan(const char *table, Opp *out, int max);
int  dtb_scan_live(const char *table, Opp *out, int max);   /* what the kernel booted with */
int  dtb_scan_factory(const char *file, const char *table, Opp *out, int max);   /* the column the tuned table came from */
int  dtb_set_uv(const char *node, int uv);
int  dtb_offset_all(const Opp *opp, int n, int offset_uv);
int  dtb_add_step(const char *table, long long hz, int uv, int bins);
int  dtb_remove_step(const char *table, long long hz);
int  dtb_has_step(const char *table, long long hz);
int  dtb_step_uv(const char *table, long long hz);
int  dtb_top_mhz(const char *table);

/* ── bench.c ───────────────────────────────────────────────────────────── */
void bench_cpu(void);
void bench_ram(void);
void bench_gpu(void);
int  bench_available_gpu(void);
void bench_history(void);
void bench_stability(void);
int  bench_passed_at(int cpu_mhz);

/* ── tabs ──────────────────────────────────────────────────────────────── */
void tab_home(Keys k, int top, int bottom);
void tab_tune(Keys k, int top, int bottom);
void tab_advanced(Keys k, int top, int bottom);
void tab_tests(Keys k, int top, int bottom);
void app_chrome(void);               /* clears the frame and draws header and tab bar; a tab calls it after handling keys */
void app_reboot_offer(const char *title, const char *msg);
void app_status_refresh(void);
extern DtbStatus app_dtb;

#endif
