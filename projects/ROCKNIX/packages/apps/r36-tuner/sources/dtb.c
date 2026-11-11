/* SPDX-License-Identifier: MIT
 * Device tree edits. All of them work on a temporary copy in /tmp: if a command fails nothing has been touched, the boot
 * partition is written once at the end by the platform, and several changes can be saved together. */
#include "tuner.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmp_path[300];
static char src_path[300];
static int dtb_flatten(void);
static int same_as_factory(void);
static int went_factory;

const char *dtb_tmp(void) { return tmp_path; }

int dtb_begin(void) {
    if (plat->dtb_source(src_path, sizeof(src_path)) != 0 || !src_path[0]) return -1;
    snprintf(tmp_path, sizeof(tmp_path), "%s", rp("/tmp/r36-tuner-edit.dtb"));
    sys_mkdirs("/tmp");
    went_factory = 0;
    if (sys_root("cp '%s' '%s' && chmod 666 '%s'", src_path, tmp_path, tmp_path)) return -1;
    if (dtb_flatten()) { dtb_abort(); return -1; }
    return 0;
}

/* A last read back: a device tree that fdtget cannot parse must never reach the boot partition */
int dtb_commit(void) {
    char b[32];
    if (dtb_flatten()) return -1;
    if (plat->single_table && same_as_factory()) {
        remove(tmp_path);
        went_factory = 1;
        return plat->dtb_revert() ? -1 : 0;
    }
    if (sys_cap(b, sizeof(b), "fdtget '%s' / compatible 2>/dev/null", tmp_path) != 0 || !b[0]) return -1;
    int rc = plat->dtb_install(tmp_path);
    remove(tmp_path);
    return rc;
}

void dtb_abort(void) { remove(tmp_path); }

static const char *read_override;
void dtb_read_from(const char *file) { read_override = file; }

/* Reads from the edit copy while editing, from the source otherwise */
static const char *reading(void) {
    if (read_override) return read_override;
    if (tmp_path[0] && access(tmp_path, F_OK) == 0) return tmp_path;
    if (plat->dtb_source(src_path, sizeof(src_path)) != 0) src_path[0] = 0;
    return src_path;
}

static const char *bin_override;
void dtb_use_bin(const char *prop) { bin_override = prop; }

/* BSP kernels print a separate column for the memory table */
const char *dtb_ram_bin(void) {
    static char prop[40];
    char r[16];
    sys_cap(r, sizeof(r), "dmesg 2>/dev/null | grep -iE 'dmc.*opp-binning' | tail -1 | grep -oE 'L[0-9]' | tail -1");
    if (r[0] == 'L') snprintf(prop, sizeof(prop), "opp-microvolt-%.2s", r);
    else snprintf(prop, sizeof(prop), "%s", dtb_bin());
    return prop;
}

/* This kernel only reads the plain column for the memory table: anything else would look saved and do nothing */
void dtb_use_ram_bin(void) {
    if (!plat->dmc_bins) { dtb_use_bin("opp-microvolt"); return; }
    const char *b = dtb_ram_bin();
    dtb_use_bin(strcmp(b, "opp-microvolt") ? b : NULL);
}

/* The chip bin: which voltage column of the table this chip uses */
const char *dtb_bin(void) {
    static char prop[40];
    char file[300], r[32] = "";
    if (bin_override) return bin_override;
    if (prop[0]) return prop;
    snprintf(file, sizeof(file), "%s/bin", plat->data_dir);
    /* The kernel measures the chip on every boot and the result can change between boots (seen on a warm restart):
       what it says now comes first, the saved value only serves when the log has already rotated */
    sys_cap(r, sizeof(r), "dmesg 2>/dev/null | grep -E 'px30_oppvariants: PVTM=[0-9]+ -> bin L[0-9]' | tail -1 | grep -oE 'L[0-9]$'");
    if (!r[0]) sys_cap(r, sizeof(r), "dmesg 2>/dev/null | grep -E 'opp-binning.*OPP prop name' | tail -1 | grep -oE 'L[0-9]' | tail -1");
    if (!r[0]) {
        sys_cap(r, sizeof(r), "dmesg 2>/dev/null | grep -oE 'pvtm-volt-sel=[0-9]' | tail -1 | grep -oE '[0-9]$'");
        if (r[0] >= '0' && r[0] <= '9') { char d = r[0]; snprintf(r, sizeof(r), "L%c", d); }
        else r[0] = 0;
    }
    if (r[0] == 'L') { char v[8]; snprintf(v, sizeof(v), "%s\n", r); sys_write(file, v); }
    else sys_read(file, r, sizeof(r));
    if (r[0] == 'L' && r[1] >= '0' && r[1] <= '9') {
        snprintf(prop, sizeof(prop), "opp-microvolt-%.2s", r);
        return prop;
    }
    return "opp-microvolt";
}

/* The chip's column even while the memory table has its own */
static const char *chip_bin(void) {
    const char *o = bin_override;
    bin_override = NULL;
    const char *b = dtb_bin();
    bin_override = o;
    return b;
}

/* Where the tuned copy says its one table came from */
#define BASE_PROP "r36-tuner,bin"
static const char *base_bin(const char *file) {
    static char prop[40];
    char b[16];
    const char *cpu = dtb_cpu_table();
    if (!cpu[0]) return NULL;
    sys_cap(b, sizeof(b), "fdtget -t s '%s' '%s' '" BASE_PROP "' 2>/dev/null", file, cpu);
    if (b[0] != 'L' || b[1] < '0' || b[1] > '9') return NULL;
    snprintf(prop, sizeof(prop), "opp-microvolt-%.2s", b);
    return prop;
}

/* One table on Zenith: the kernel measures the chip on every boot and may pick another column, by temperature too, so a change
   that was tested would not be what runs. The CPU and GPU steps get the column of the boot the first edit started on as their
   only opp-microvolt, every cell as it is, and the L columns go; the kernel reads opp-microvolt when the column it picked is
   not there. The factory device tree keeps them. Done again before saving: the extra CPU speeds are written with L columns. */
static int flatten_file(const char *file, const char *col) {
    const char *const tables[] = {dtb_cpu_table(), dtb_gpu_table()};
    char tb[2][48];
    for (int i = 0; i < 2; i++) snprintf(tb[i], sizeof(tb[i]), "%s", tables[i]);
    for (int i = 0; i < 2; i++) {
        if (!tb[i][0]) continue;
        if (sys_root("f='%s'; t='%s'; for n in $(fdtget -l \"$f\" \"$t\"); do case $n in opp-*|opp@*) ;; *) continue;; esac; "
                     "p=\"$t/$n\"; v=$(fdtget -t u \"$f\" \"$p\" '%s' 2>/dev/null); "
                     "if [ -n \"$v\" ]; then fdtput -t u \"$f\" \"$p\" opp-microvolt $v || exit 1; fi; "
                     "for c in L0 L1 L2 L3; do if fdtget \"$f\" \"$p\" opp-microvolt-$c >/dev/null 2>&1; then "
                     "fdtput -d \"$f\" \"$p\" opp-microvolt-$c || exit 1; fi; done; done",
                     file, tb[i], col)) return -1;
    }
    return 0;
}

static int dtb_flatten(void) {
    if (!plat->single_table) return 0;
    const char *col = base_bin(tmp_path);
    if (!col) col = chip_bin();
    if (!strcmp(col, "opp-microvolt")) return 0;   /* chip not known: nothing safe to freeze */
    char c[40];
    snprintf(c, sizeof(c), "%s", col);   /* base_bin's buffer is reused */
    if (flatten_file(tmp_path, c)) return -1;
    return sys_root("fdtput -t s '%s' '%s' '" BASE_PROP "' '%.2s'", tmp_path, dtb_cpu_table(), c + 14) ? -1 : 0;
}

/* Every step of the three tables, every property and value: a fingerprint to compare two device trees. Of a voltage only the
   target counts: an edit writes the same value three times, factory has a higher maximum */
static int tables_md5(const char *file, char *out, int n) {
    char tb[3][48];
    snprintf(tb[0], sizeof(tb[0]), "%s", dtb_cpu_table());
    snprintf(tb[1], sizeof(tb[1]), "%s", dtb_gpu_table());
    snprintf(tb[2], sizeof(tb[2]), "%s", dtb_ram_table());
    return sys_cap(out, n, "f='%s'; for t in '%s' '%s' '%s'; do [ -n \"$t\" ] || continue; "
                   "for m in $(fdtget -l \"$f\" \"$t\" 2>/dev/null); do for p in $(fdtget -p \"$f\" \"$t/$m\" | sort); do "
                   "v=$(fdtget -t x \"$f\" \"$t/$m\" $p); case $p in opp-microvolt*) v=${v%%%% *};; esac; "
                   "echo \"$t/$m $p $v\"; done; done; done | md5sum",
                   file, tb[0], tb[1], tb[2]);
}

/* The editor's Factory on every step leaves the one table equal to the factory column it came from: nothing is left to keep,
   so the factory device tree comes back, with the columns the kernel chooses from */
static int same_as_factory(void) {
    char fac[300], ff[300], col[40], a[64], b[64];
    const char *base = base_bin(tmp_path);
    if (!base || plat->dtb_factory(fac, sizeof(fac)) || !fac[0]) return 0;
    snprintf(col, sizeof(col), "%s", base);
    snprintf(ff, sizeof(ff), "%s", rp("/tmp/r36-tuner-factory.dtb"));
    if (sys_root("cp '%s' '%s' && chmod 666 '%s'", fac, ff, ff)) return 0;
    int same = !flatten_file(ff, col) && !tables_md5(tmp_path, a, sizeof(a)) && !tables_md5(ff, b, sizeof(b)) && a[0] && !strcmp(a, b);
    remove(ff);
    return same;
}

int dtb_went_factory(void) { return went_factory; }

int dtb_scan_factory(const char *file, const char *table, Opp *out, int max) {
    const char *base = strcmp(table, dtb_ram_table()) ? base_bin(reading()) : NULL;
    const char *o = bin_override;
    if (base) bin_override = base;
    dtb_read_from(file);
    int n = dtb_scan(table, out, max);
    dtb_read_from(NULL);
    bin_override = o;
    return n;
}

static const char *find_table(const char *const *cands, char *out, int n) {
    char r[16];
    const char *d = reading();
    out[0] = 0;
    for (int i = 0; cands[i]; i++) {
        sys_cap(r, sizeof(r), "fdtget '%s' '%s' compatible 2>/dev/null | head -c 4", d, cands[i]);
        if (r[0]) { snprintf(out, n, "%s", cands[i]); break; }
    }
    return out;
}

const char *dtb_cpu_table(void) {
    static char t[48];
    static const char *const c[] = {"/opp-table-0", "/cpu0-opp-table", "/cpu-opp-table", "/opp-table0", "/opp-table", NULL};
    return find_table(c, t, sizeof(t));
}

const char *dtb_gpu_table(void) {
    static char t[48];
    static const char *const c[] = {"/opp-table-1", "/gpu-opp-table", "/gpu_opp_table", "/gpu-opp-table-0", NULL};
    return find_table(c, t, sizeof(t));
}

const char *dtb_ram_table(void) {
    static char t[48];
    static const char *const c[] = {"/dmc_opp_table", "/dmc-opp-table", NULL};
    return find_table(c, t, sizeof(t));
}

static int first_uv(const char *d, const char *node, const char *prop) {
    char b[64];
    sys_cap(b, sizeof(b), "fdtget -t u '%s' '%s' '%s' 2>/dev/null | awk '{print $1}'", d, node, prop);
    return b[0] ? atoi(b) : 0;
}

int dtb_scan(const char *table, Opp *out, int max) {
    char list[1200], *names[MAX_OPP];
    const char *d = reading();
    const char *bin = dtb_bin();
    sys_cap(list, sizeof(list), "fdtget -l '%s' '%s' 2>/dev/null", d, table);
    int nn = sys_split(list, names, MAX_OPP), n = 0;
    for (int i = 0; i < nn && n < max; i++) {
        if (strncmp(names[i], "opp-", 4) && strncmp(names[i], "opp@", 4)) continue;
        long long hz = atoll(names[i] + 4);
        if (!hz) continue;
        char node[160];
        snprintf(node, sizeof(node), "%s/%s", table, names[i]);
        int uv = first_uv(d, node, bin);
        if (!uv) uv = first_uv(d, node, "opp-microvolt");
        if (uv < 100000 || uv > 2000000) continue;
        snprintf(out[n].node, sizeof(out[n].node), "%s", node);
        out[n].hz = hz;
        out[n].uv = uv;
        n++;
    }
    /* by speed, highest first */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j].hz > out[i].hz) { Opp t = out[i]; out[i] = out[j]; out[j] = t; }
    return n;
}

int dtb_scan_live(const char *table, Opp *out, int max) {
    char base[200];
    snprintf(base, sizeof(base), "/proc/device-tree%s", table);
    DIR *d = opendir(rp(base));
    if (!d) return 0;
    const char *bin = dtb_bin();
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, "opp-", 4) && strncmp(e->d_name, "opp@", 4)) continue;
        long long hz = atoll(e->d_name + 4);
        if (!hz) continue;
        char p[400];
        snprintf(p, sizeof(p), "%s/%s/%s", base, e->d_name, bin);
        int fd = open(rp(p), O_RDONLY);
        if (fd < 0) {
            snprintf(p, sizeof(p), "%s/%s/opp-microvolt", base, e->d_name);
            fd = open(rp(p), O_RDONLY);
        }
        if (fd < 0) continue;
        unsigned char b[4];
        ssize_t r = read(fd, b, 4);
        close(fd);
        if (r < 4) continue;
        out[n].hz = hz;
        out[n].uv = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        out[n].node[0] = 0;
        n++;
    }
    closedir(d);
    return n;
}

/* How many values the property has (1, or min typ max) and the first one */
static int read_cells(const char *node, const char *prop, int *first) {
    char b[80], *t[4];
    sys_cap(b, sizeof(b), "fdtget -t u '%s' '%s' '%s' 2>/dev/null", tmp_path, node, prop);
    int cells = sys_split(b, t, 4);
    *first = cells ? atoi(t[0]) : 0;
    return cells;
}

static int put_uv(const char *node, const char *prop, int cells, int uv) {
    if (cells == 3) return sys_root("fdtput -t u '%s' '%s' '%s' %d %d %d", tmp_path, node, prop, uv, uv, uv);
    if (cells == 2) return sys_root("fdtput -t u '%s' '%s' '%s' %d %d", tmp_path, node, prop, uv, uv);
    return sys_root("fdtput -t u '%s' '%s' '%s' %d", tmp_path, node, prop, uv);
}

/* The ceiling of the rail this step runs on: CPU, or GPU and RAM, which share one */
static int rail_max_uv(const char *node) {
    const char *cpu = dtb_cpu_table();
    size_t n = strlen(cpu);
    return n && !strncmp(node, cpu, n) && node[n] == '/' ? CPU_MAX_UV : LOGIC_MAX_UV;
}

/* One table (dtb_flatten): only opp-microvolt. Otherwise writes the chip's column, and moves the others by the same amount: the
   kernel may pick another column on the next boot, and a change has to survive that. Never below the chosen voltage, which is
   the one that was tested, nor above the rail's ceiling */
int dtb_set_uv(const char *node, int uv) {
    static const char *const cols[] = {"opp-microvolt-L0", "opp-microvolt-L1", "opp-microvolt-L2", "opp-microvolt-L3", "opp-microvolt"};
    const char *bin = dtb_bin();
    const int top = rail_max_uv(node);
    int old, cells = read_cells(node, bin, &old);
    if (uv > top) uv = top;
    if (!cells && plat->single_table) return put_uv(node, "opp-microvolt", read_cells(node, "opp-microvolt", &old), uv) ? -1 : 0;
    int delta = cells ? uv - old : 0;
    if (put_uv(node, bin, cells, uv)) return -1;
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        int v;
        if (!strcmp(cols[i], bin)) continue;
        int c = read_cells(node, cols[i], &v);
        if (!c) continue;
        v += delta;
        if (v < uv) v = uv;
        if (v > top) v = top;
        if (put_uv(node, cols[i], c, v)) return -1;
    }
    return 0;
}

int dtb_offset_all(const Opp *opp, int n, int offset_uv) {
    for (int i = 0; i < n; i++) {
        int uv = opp[i].uv + offset_uv;
        if (uv < OPP_FLOOR_UV) uv = OPP_FLOOR_UV;
        if (uv != opp[i].uv && dtb_set_uv(opp[i].node, uv)) return -1;
    }
    return 0;
}

static void step_node(char *out, int n, const char *table, long long hz) { snprintf(out, n, "%s/opp-%lld", table, hz); }

int dtb_add_step(const char *table, long long hz, int uv, int bins) {
    char node[160];
    step_node(node, sizeof(node), table, hz);
    if (sys_root("fdtput -c '%s' '%s'", tmp_path, node)) return -1;
    if (sys_root("fdtput -t u '%s' '%s' opp-hz %lld %lld", tmp_path, node, hz >> 32, hz & 0xFFFFFFFFLL)) return -1;
    if (sys_root("fdtput -t u '%s' '%s' opp-microvolt %d", tmp_path, node, uv)) return -1;
    if (bins && !plat->single_table) {
        const char *bin = dtb_bin();
        if (strcmp(bin, "opp-microvolt") && sys_root("fdtput -t u '%s' '%s' '%s' %d", tmp_path, node, bin, uv)) return -1;
    }
    return 0;
}

int dtb_remove_step(const char *table, long long hz) {
    char node[160];
    step_node(node, sizeof(node), table, hz);
    return sys_root("fdtput -r '%s' '%s'", tmp_path, node) ? -1 : 0;
}

int dtb_has_step(const char *table, long long hz) {
    char r[16], node[160];
    step_node(node, sizeof(node), table, hz);
    sys_cap(r, sizeof(r), "fdtget '%s' '%s' opp-hz 2>/dev/null | head -c 4", reading(), node);
    return r[0] != 0;
}

int dtb_step_uv(const char *table, long long hz) {
    char node[160];
    step_node(node, sizeof(node), table, hz);
    const char *d = reading();
    int uv = first_uv(d, node, dtb_bin());
    return uv ? uv : first_uv(d, node, "opp-microvolt");
}

int dtb_top_mhz(const char *table) {
    Opp o[MAX_OPP];
    int n = dtb_scan(table, o, MAX_OPP);
    return n ? (int)(o[0].hz / 1000000) : 0;
}
