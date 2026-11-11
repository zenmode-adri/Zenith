/* SPDX-License-Identifier: MIT
 * What differs between Zenith (ROCKNIX, kernel 6.12, root, /flash read only, extlinux) and dArkOSRE (BSP kernel,
 * user ark + sudo, /boot writable, boot.ini). Everything else in the program is shared and asks for capabilities. */
#include "tuner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const Platform *plat;

/* ── Clock limits that survive a reboot ─────────────────────────────────── */
/* limits file: "<cpu_khz> <gpu_hz> <ram_hz>"; r36-tuner-limits writes it back at boot */
void limits_save(void) {
    char p[300], v[96];
    snprintf(p, sizeof(p), "%s/limits", plat->data_dir);
    snprintf(v, sizeof(v), "%d %lld %lld\n", sys_int(CPU_POLICY "/scaling_max_freq"),
             sys_ll(GPU_DEVFREQ "/max_freq"), sys_ll(DMC_DEVFREQ "/max_freq"));
    sys_write(p, v);
}

/* ── Zenith ─────────────────────────────────────────────────────────────── */
static int z_source(char *out, int n) { return sys_cap(out, n, "r36-dtb source 2>/dev/null | tail -1"); }
static int z_factory(char *out, int n) {
    char name[160];
    sys_cap(name, sizeof(name), "r36-dtb status 2>/dev/null | sed -n 's/^factory=//p'");
    if (!name[0]) return z_source(out, n);
    char p[200];
    snprintf(p, sizeof(p), "/flash/%s", name);
    snprintf(out, n, "%s", rp(p));
    return 0;
}
static int z_install(const char *f) { return sys_root("r36-dtb install '%s'", f); }
static int z_revert(void) { return sys_root("r36-dtb revert"); }
static int z_trial(int on) { return sys_root("r36-dtb %s", on == 1 ? "test-start" : on == 2 ? "test-end failed" : "test-end"); }
static int z_confirm(void) { return sys_root("r36-dtb confirm"); }
static void z_ack(void) { sys_root("r36-dtb ack"); }

static void z_status(DtbStatus *s) {
    char b[300];
    memset(s, 0, sizeof(*s));
    sys_cap(b, sizeof(b), "r36-dtb status 2>/dev/null");
    s->tuned = strstr(b, "fdt=tuned.dtb") != NULL;
    s->reboot_needed = strstr(b, "state=pending") != NULL;
    if (strstr(b, "state=pending")) s->net = NET_PENDING;
    else if (strstr(b, "state=trial") || strstr(b, "state=testing")) s->net = NET_TRIAL;
    else if (strstr(b, "state=ok")) s->net = NET_OK;
    else if (strstr(b, "state=failed")) s->net = NET_UNDONE;
    else if (strstr(b, "state=stale")) s->net = NET_STALE;
}

static void z_set_cpu_gov(const char *g) {
    /* system.cpugovernor is what runemu.sh applies around every game; its function also sets the memory governor */
    sys_root("bash -c '. %s; set_setting system.cpugovernor %s; %s'", rp("/etc/profile"), g, g);
    char now[32];
    sys_read(CPU_POLICY "/scaling_governor", now, sizeof(now));
    if (strcmp(now, g)) sys_write(CPU_POLICY "/scaling_governor", g);
}

static void z_set_gpu_mode(const char *m) {
    sys_root("bash -c '. %s; set_setting system.gpuperf %s; gpu_performance_level %s'", rp("/etc/profile"), m, m);
}

static void z_set_ram_gov(const char *g) { sys_write(DMC_DEVFREQ "/governor", g); }

/* powerstate gives the GPU the level in this file every time the battery changes state; runemu.sh uses it during a game */
#define Z_GPU_LEVEL "/tmp/.gpu_performance_level"
static void z_hold_gpu(int on) {
    static char before[32];
    static int had;
    if (on) {
        had = sys_read(Z_GPU_LEVEL, before, sizeof(before)) == 0;
        sys_write(Z_GPU_LEVEL, "performance\n");
    } else if (had) {
        char v[40];
        snprintf(v, sizeof(v), "%s\n", before);
        sys_write(Z_GPU_LEVEL, v);
    } else {
        sys_root("rm -f '%s'", rp(Z_GPU_LEVEL));
    }
}

static void z_perf_get(char *out, int n) {
    sys_cap(out, n, "bash -c '. %s; get_setting global.perfmode' 2>/dev/null", rp("/etc/profile"));
}

static void z_perf_set(const char *v) {
    sys_root("bash -c '. %s; set_setting global.perfmode %s'", rp("/etc/profile"), v);
}

static const char *z_lang(void) {
    static char b[32];
    sys_cap(b, sizeof(b), "bash -c '. %s; get_setting system.language' 2>/dev/null", rp("/etc/profile"));
    if (b[0]) { b[2] = 0; return b; }
    return NULL;
}

static void z_reboot(void) { sys_root("sync; systemctl reboot || reboot"); }

static const char *z_gpu_bench(void) {
    if (access("/usr/bin/glmark2-es2-wayland", X_OK) == 0 || getenv("R36T_ROOT"))
        return ". /etc/profile; glmark2-es2-wayland --off-screen --size 320x240 "
               "-b build:duration=15 -b texture:duration=15 -b shading:duration=15 -b terrain:duration=15";
    return NULL;
}

static const char *const z_fonts[] = {
    "/usr/share/themes/es-theme-zenith/assets/fonts/Rubik-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    NULL };

/* 786 is stock on the R36S boards and the Ultra, added here on other models; not every unit has been seen running it.
   924 is what AURKNIX ships, on top of 786. */
static const RamStep z_ram[] = {
    { 786, 786, 0, 1100000, 1 },
    { 924, 924, 786, 1125000, 1 },
};

static const Platform P_ZENITH = {
    .id = "zenith", .name = "Zenith",
    .data_dir = "/storage/.config/r36-tuner",
    .fonts = z_fonts,
    .has_boost = 1, .has_perfmode = 1, .joint_ram_gov = 1, .single_table = 1,
    .ram_steps = z_ram, .n_ram_steps = 2,
    .dtb_source = z_source, .dtb_factory = z_factory, .dtb_install = z_install, .dtb_revert = z_revert, .dtb_status = z_status,
    .dtb_trial = z_trial, .dtb_confirm = z_confirm, .dtb_ack = z_ack,
    .recovery = {
        "Power off the console and remove the SD card.",
        "Insert it in a PC and open the FAT partition. R36-TUNER-RECOVERY.txt there has these same steps.",
        "Copy tuner/extlinux.conf.factory over extlinux/extlinux.conf.",
        "Optional, to also turn off Turbo and frequency limits: create an empty file named r36-tuner-reset in that partition.",
        "Reinsert the SD card and power on the console.",
        NULL },
    .set_cpu_gov = z_set_cpu_gov, .set_gpu_mode = z_set_gpu_mode, .set_ram_gov = z_set_ram_gov,
    .perfmode_get = z_perf_get, .perfmode_set = z_perf_set,
    .save_limits = limits_save, .hold_gpu = z_hold_gpu,
    .language = z_lang, .reboot = z_reboot, .gpu_bench_cmd = z_gpu_bench,
    .bench_dir = "/usr/bin",
};

/* ── dArkOSRE ───────────────────────────────────────────────────────────── */
#define D_PENDING "/boot/.r36_dtb_patch_pending"
#define D_BOOTING "/boot/.r36_dtb_patch_booting"

static const char *d_dtb(void) {
    static char path[200];
    const char *const cand[] = { "/boot/rk3326-r36s-linux.dtb", "/boot/rk3326-r36s.dtb", NULL };
    char name[120], one[160];
    /* the one boot.ini loads, when it names a single one that is there */
    sys_cap(name, sizeof(name), "grep -oE 'rk3326[-A-Za-z0-9_.]*\\.dtb' '%s' 2>/dev/null | sort -u | { read -r a; read -r b; [ -z \"$b\" ] && echo \"$a\"; }", rp("/boot/boot.ini"));
    if (name[0]) {
        snprintf(one, sizeof(one), "/boot/%s", name);
        if (sys_exists(one)) { snprintf(path, sizeof(path), "%s", rp(one)); return path; }
    }
    for (int i = 0; cand[i]; i++)
        if (sys_exists(cand[i])) { snprintf(path, sizeof(path), "%s", rp(cand[i])); return path; }
    /* any other only if there is no doubt: editing one that does not boot would look saved and do nothing */
    sys_cap(path, sizeof(path), "ls '%s'/rk3326*.dtb 2>/dev/null | grep -v tuned | { read -r a; read -r b; [ -z \"$b\" ] && echo \"$a\"; }", rp("/boot"));
    return path;
}

static int d_source(char *out, int n) {
    const char *p = d_dtb();
    if (!p[0]) return -1;
    snprintf(out, n, "%s", p);
    return 0;
}

/* Installs (once) the two services that undo a patched device tree when a boot does not finish */
static int d_setup_safety(void) {
    const char *dtb = d_dtb();
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", rp("/tmp/r36_safety_setup.sh"));
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
        "#!/bin/bash\n"
        "cat > /usr/local/bin/r36-dtb-safety.sh << 'SAFEEOF'\n"
        "#!/bin/bash\n"
        "DTB=%s\n"
        "BOOTING=" D_BOOTING "\n"
        "PENDING=" D_PENDING "\n"
        "if [ -f \"$BOOTING\" ]; then\n"
        "  [ -f \"$DTB.bak\" ] && cp \"$DTB.bak\" \"$DTB\"\n"
        "  rm -f \"$BOOTING\" \"$PENDING\"\n"
        "  touch /boot/.r36_dtb_undone\n"
        "  sync\n"
        "  systemctl reboot --no-block 2>/dev/null || reboot -f\n"   /* this boot already loaded the tuned device tree */
        "  exit 0\n"
        "fi\n"
        "if [ -f \"$PENDING\" ]; then\n"
        "  mv \"$PENDING\" \"$BOOTING\" && sync\n"
        "fi\n"
        "exit 0\n"
        "SAFEEOF\n"
        "chmod +x /usr/local/bin/r36-dtb-safety.sh\n"
        "printf '[Unit]\\nDescription=R36 DTB safety\\n"
            "DefaultDependencies=no\\nAfter=local-fs.target\\nBefore=basic.target\\n\\n"
            "[Service]\\nType=oneshot\\n"
            "ExecStart=/usr/local/bin/r36-dtb-safety.sh\\nRemainAfterExit=yes\\n\\n"
            "[Install]\\nWantedBy=basic.target\\n'"
        " > /etc/systemd/system/r36-dtb-safety.service\n"
        "cat > /usr/local/bin/r36-dtb-ok.sh << 'OKEOF'\n"
        "#!/bin/bash\n"
        "rm -f " D_BOOTING "\n"
        "sync\n"
        "OKEOF\n"
        "chmod +x /usr/local/bin/r36-dtb-ok.sh\n"
        "printf '[Unit]\\nDescription=R36 DTB boot OK\\nAfter=emulationstation.service\\n\\n"
            "[Service]\\nType=oneshot\\n"
            "ExecStart=/usr/local/bin/r36-dtb-ok.sh\\nRemainAfterExit=yes\\n\\n"
            "[Install]\\nWantedBy=multi-user.target\\n'"
        " > /etc/systemd/system/r36-dtb-ok.service\n"
        "systemctl daemon-reload 2>/dev/null\n"
        "systemctl enable r36-dtb-safety.service 2>/dev/null\n"
        "systemctl enable r36-dtb-ok.service 2>/dev/null\n", dtb);
    fclose(f);
    if (getenv("R36T_ROOT")) return getenv("R36T_FAIL_SAFETY") ? -1 : 0;   /* tests: the service cannot be installed there */
    return sys_root("bash %s", tmp);
}

/* The steps for a PC, on the partition a PC can read */
static int d_write_note(const char *dtb) {
    const char *name = strrchr(dtb, '/');
    name = name ? name + 1 : dtb;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", rp("/tmp/R36-TUNER-RECOVERY.txt"));
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
        "R36 Tuner: recovery from a PC / recuperacion desde un PC\n\n"
        "English\n"
        "If the console does not boot after changing voltages or frequencies:\n"
        "1. Power off the console and remove the SD card.\n"
        "2. Insert it in a PC and open this partition.\n"
        "3. Copy %s.bak over %s (replace the file).\n"
        "4. If you used the CPU overclock, copy boot.ini.oc_bak over boot.ini too.\n"
        "5. Reinsert the SD card and power on the console.\n\n"
        "Espanol\n"
        "Si la consola no arranca tras cambiar voltajes o frecuencias:\n"
        "1. Apaga la consola y extrae la tarjeta SD.\n"
        "2. Insertala en un PC y abre esta particion.\n"
        "3. Copia %s.bak encima de %s (sustituye el fichero).\n"
        "4. Si usaste el overclock de CPU, copia tambien boot.ini.oc_bak encima de boot.ini.\n"
        "5. Vuelve a insertar la tarjeta SD y enciende la consola.\n", name, name, name, name);
    fclose(f);
    return sys_root("cp '%s' '%s'", tmp, rp("/boot/R36-TUNER-RECOVERY.txt"));
}

/* The working file is ours only while it is what we left there: otherwise a system update wrote it, and it is the new original.
   An older version of this program left no record: a backup that differs is then the only sign, and the backup has to stay */
static int d_is_ours(const char *dtb) {
    char ours[80], now[80], diff[8];
    sys_cap(ours, sizeof(ours), "cat '%s.tuned.sha' 2>/dev/null", dtb);
    if (!ours[0]) {
        sys_cap(diff, sizeof(diff), "[ -f '%s.bak' ] && ! cmp -s '%s' '%s.bak' && echo y", dtb, dtb, dtb);
        return diff[0] == 'y';
    }
    sys_cap(now, sizeof(now), "sha256sum '%s' 2>/dev/null | cut -d' ' -f1", dtb);
    return !strcmp(ours, now);
}

static int d_install(const char *edited) {
    const char *dtb = d_dtb();
    if (!dtb[0]) return -1;
    if (d_setup_safety()) return -1;
    /* the backup is made once and checked; a new one only when an update replaced the original */
    if (!d_is_ours(dtb) && sys_root("rm -f '%s.bak'", dtb)) return -1;
    if (sys_root("[ -f '%s.bak' ] || { cp '%s' '%s.bak.new' && cmp -s '%s' '%s.bak.new' && mv '%s.bak.new' '%s.bak'; }", dtb, dtb, dtb, dtb, dtb, dtb, dtb)) return -1;
    if (sys_root("[ -s '%s.bak' ]", dtb)) return -1;
    if (d_write_note(dtb)) return -1;
    /* The mark goes first: what boots next does not change without it on the card */
    if (sys_root("cp '%s' '%s.new' && cmp -s '%s' '%s.new'", edited, dtb, edited, dtb)) return -1;
    if (sys_root("touch '%s' && rm -f '%s' && sync", rp(D_PENDING), rp("/boot/.r36_dtb_undone")) ||
        sys_root("sha256sum '%s.new' | cut -d' ' -f1 >'%s.tuned.sha' && mv '%s.new' '%s' && sync", dtb, dtb, dtb, dtb)) {
        sys_root("rm -f '%s' '%s.new'; sync", rp(D_PENDING), dtb);
        return -1;
    }
    return 0;
}

static int d_revert(void) {
    const char *dtb = d_dtb();
    const char *bi = rp("/boot/boot.ini");
    sys_root("[ -f '%s.oc_bak' ] && cp '%s.oc_bak' '%s'; rm -f '%s' '%s'", bi, bi, bi, rp("/boot/.r36_oc_boot_pending"), rp("/boot/.r36_oc_boot_booting"));
    if (!d_is_ours(dtb)) return sys_root("rm -f '%s' '%s'; sync", rp(D_PENDING), rp(D_BOOTING));   /* nothing of ours to take back */
    return sys_root("[ -f '%s.bak' ] && cp '%s.bak' '%s' && rm -f '%s' '%s' '%s.tuned.sha' && sync",
                    dtb, dtb, dtb, rp(D_PENDING), rp(D_BOOTING), dtb);
}

static void d_status(DtbStatus *s) {
    memset(s, 0, sizeof(*s));
    const char *dtb = d_dtb();
    s->tuned = d_is_ours(dtb);
    {
        char same[8], bi[300];
        snprintf(bi, sizeof(bi), "%s", rp("/boot/boot.ini"));
        sys_cap(same, sizeof(same), "[ -f '%s.oc_bak' ] && ! cmp -s '%s' '%s.oc_bak' && echo y", bi, bi, bi);
        s->overclocked = same[0] == 'y';
    }
    if (sys_exists(D_PENDING)) { s->net = NET_PENDING; s->reboot_needed = 1; }
    else if (sys_exists("/boot/.r36_dtb_undone")) s->net = NET_UNDONE;
    else if (s->tuned) s->net = NET_OK;
}

/* While the flag is there, a crash undoes the tuning at the next boot (r36-dtb-safety.sh) */
static int d_trial(int on) {
    return on == 1 ? sys_root("touch '%s'; sync", rp(D_BOOTING)) : sys_root("rm -f '%s'; sync", rp(D_BOOTING));
}
static int d_confirm(void) { return 0; }
static void d_ack(void) { sys_root("rm -f '%s'; sync", rp("/boot/.r36_dtb_undone")); }

/* The limits are put back by a small service, installed the first time they are saved */
static void d_save_limits(void) {
    limits_save();
    if (sys_exists("/etc/systemd/system/r36-tuner-limits.service") || getenv("R36T_ROOT")) return;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", rp("/tmp/r36_limits_setup.sh"));
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f,
        "#!/bin/bash\n"
        "printf '[Unit]\\nDescription=R36 Tuner clock limits\\nAfter=multi-user.target\\n\\n"
            "[Service]\\nType=oneshot\\n"
            "ExecStart=/usr/local/bin/r36-tuner-limits %s/limits\\n\\n"
            "[Install]\\nWantedBy=multi-user.target\\n'"
        " > /etc/systemd/system/r36-tuner-limits.service\n"
        "systemctl daemon-reload 2>/dev/null\n"
        "systemctl enable r36-tuner-limits.service 2>/dev/null\n", plat->data_dir);
    fclose(f);
    sys_root("bash %s", tmp);
}

static void d_set_cpu_gov(const char *g) { sys_write(CPU_POLICY "/scaling_governor", g); }

/* The governor names of this kernel carry a prefix: simple_ondemand, dmc_ondemand */
static void d_gov(const char *dev, const char *g) {
    char p[160], avail[300], v[48];
    snprintf(p, sizeof(p), "%s/available_governors", dev);
    sys_read(p, avail, sizeof(avail));
    const char *const pre[] = { "", "simple_", "dmc_", NULL };
    snprintf(p, sizeof(p), "%s/governor", dev);
    for (int i = 0; pre[i]; i++) {
        snprintf(v, sizeof(v), "%s%s", pre[i], g);
        if (sys_has_word(avail, v)) { sys_write(p, v); return; }
    }
}
static void d_set_gpu_mode(const char *m) { d_gov(GPU_DEVFREQ, strcmp(m, "auto") ? m : "ondemand"); }
static void d_set_ram_gov(const char *g) { d_gov(DMC_DEVFREQ, g); }

static void d_reboot(void) { sys_root("sync; /sbin/reboot"); }

static const char *d_gpu_bench(void) {
    if (access("/usr/local/bin/glmark2-es2-drm-legacy", X_OK) != 0 && !getenv("R36T_ROOT")) return NULL;
    return "export SDL_VIDEO_EGL_DRIVER=/lib/aarch64-linux-gnu/libmali-bifrost-g31-rxp0-gbm.so; "
           "/usr/local/bin/glmark2-es2-drm-legacy --off-screen --data-path /usr/local/share/glmark2data --size 320x240 "
           "-b build:duration=15 -b texture:duration=15 -b shading:duration=15 -b terrain:duration=15";
}

static int d_factory(char *out, int n) {
    const char *p = d_dtb();
    if (!p[0]) return -1;
    char bak[300];
    snprintf(bak, sizeof(bak), "%s.bak", p);
    snprintf(out, n, "%s", access(bak, F_OK) == 0 ? bak : p);
    return 0;
}

static const char *const d_fonts[] = {
    "/usr/local/share/r36-tuner/Rubik-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL };

/* 928 is a request the memory answers with ~924; the 1040 node with 1032 (ATF) */
static const RamStep d_ram[] = {
    { 928, 924, 0, 1100000, 0 },
    { 1040, 1032, 928, 1150000, 1 },
};

static const RamStep d_gpu[] = { { 600, 600, 0, 1050000, 0 } };

static const Platform P_DARKOSRE = {
    .id = "darkosre", .name = "dArkOSRE",
    .data_dir = "/home/ark/.config/r36-tuner",
    .fonts = d_fonts,
    .has_bootini_oc = 1, .has_lang_menu = 1, .fake_oc_check = 1, .dmc_bins = 1,
    .ram_steps = d_ram, .n_ram_steps = 2,
    .gpu_steps = d_gpu, .n_gpu_steps = 1,
    .dtb_source = d_source, .dtb_factory = d_factory, .dtb_install = d_install, .dtb_revert = d_revert, .dtb_status = d_status,
    .dtb_trial = d_trial, .dtb_confirm = d_confirm, .dtb_ack = d_ack,
    .recovery = {
        "Power off the console and remove the SD card.",
        "Insert it in a PC and open the FAT partition (/boot). R36-TUNER-RECOVERY.txt there has these same steps.",
        "Copy the file ending in .dtb.bak over the file with the same name without .bak.",
        "If you used the CPU overclock, also copy boot.ini.oc_bak over boot.ini.",
        "Reinsert the SD card and power on the console.",
        NULL },
    .set_cpu_gov = d_set_cpu_gov, .set_gpu_mode = d_set_gpu_mode, .set_ram_gov = d_set_ram_gov,
    .save_limits = d_save_limits,
    .reboot = d_reboot, .gpu_bench_cmd = d_gpu_bench,
    .bench_dir = "/usr/local/bin",
};

/* ── Detection ──────────────────────────────────────────────────────────── */
void plat_init(void) {
    const char *force = getenv("R36T_PLATFORM");
    if (force) { plat = strcmp(force, "darkosre") == 0 ? &P_DARKOSRE : &P_ZENITH; }
    else {
        char b[1000];
        sys_read("/etc/os-release", b, sizeof(b));
        plat = (strstr(b, "ROCKNIX") || strstr(b, "Zenith") || strstr(b, "AURKNIX") || sys_exists("/flash/extlinux/extlinux.conf"))
               ? &P_ZENITH : &P_DARKOSRE;
    }
    sys_mkdirs(plat->data_dir);
}

/* Benchmarks live in the system folder once installed; while testing, next to this executable */
const char *bench_path(const char *name) {
    static char buf[2][320];
    static int slot;
    char *b = buf[slot ^= 1];
    snprintf(b, 320, "%s/%s", plat->bench_dir, name);
    const char *r = rp(b);
    /* without a simulated root rp() hands back b itself, and copying it over itself leaves an empty path */
    if (access(r, X_OK) == 0) { if (r != b) snprintf(b, 320, "%s", r); return b; }
    char self[256];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        char *sl = strrchr(self, '/');
        if (sl) {
            *sl = 0;
            snprintf(b, 320, "%s/%s", self, name);
            if (access(b, X_OK) == 0) return b;
        }
    }
    snprintf(b, 320, "%s/%s", plat->bench_dir, name);
    return b;
}

/* ── Profiles ───────────────────────────────────────────────────────────── */
/* Top of a table in a sysfs list, biggest first or last */
static long long list_max(const char *path, long long below) {
    char b[400];
    char *t[32];
    sys_read(path, b, sizeof(b));
    int n = sys_split(b, t, 32);
    long long best = 0;
    for (int i = 0; i < n; i++) {
        long long v = atoll(t[i]);
        if (v > best && (below == 0 || v <= below)) best = v;
    }
    return best;
}

static void set_limits(long long cpu_khz, long long gpu_hz, long long ram_hz) {
    char v[32];
    if (cpu_khz) { snprintf(v, sizeof(v), "%lld", cpu_khz); sys_write(CPU_POLICY "/scaling_max_freq", v); }
    if (gpu_hz)  { snprintf(v, sizeof(v), "%lld", gpu_hz);  sys_write(GPU_DEVFREQ "/max_freq", v); }
    if (ram_hz)  { snprintf(v, sizeof(v), "%lld", ram_hz);  sys_write(DMC_DEVFREQ "/max_freq", v); }
    plat->save_limits();
}

static void profile_file(char *p, int n) { snprintf(p, n, "%s/profile", plat->data_dir); }

Profile profile_current(void) {
    char p[300], b[16];
    profile_file(p, sizeof(p));
    if (sys_read(p, b, sizeof(b))) return PROFILE_BALANCED;
    if (!strcmp(b, "save")) return PROFILE_SAVE;
    if (!strcmp(b, "perf")) return PROFILE_PERF;
    if (!strcmp(b, "custom")) return PROFILE_CUSTOM;
    return PROFILE_BALANCED;
}

/* On Zenith the governors are reapplied by runemu.sh around every game, so a profile goes through the settings it reads:
   system.cpugovernor and global.perfmode. Only the clock limits are written directly, and saved for the next boot. */
void profile_apply(Profile want) {
    char p[300];
    long long cpu = list_max(CPU_POLICY "/scaling_available_frequencies", 0);
    long long gpu = list_max(GPU_DEVFREQ "/available_frequencies", 0);
    long long ram = list_max(DMC_DEVFREQ "/available_frequencies", 0);
    profile_file(p, sizeof(p));
    switch (want) {
    case PROFILE_SAVE:
        plat->set_cpu_gov("ondemand");
        plat->set_gpu_mode("auto");
        if (plat->perfmode_set) plat->perfmode_set("0");
        set_limits(list_max(CPU_POLICY "/scaling_available_frequencies", 1008000),
                   list_max(GPU_DEVFREQ "/available_frequencies", 400000000), ram);
        sys_write(p, "save\n");
        break;
    case PROFILE_PERF:
        set_limits(cpu, gpu, ram);
        plat->set_cpu_gov("performance");
        if (plat->perfmode_set) plat->perfmode_set("1");
        else { plat->set_gpu_mode("performance"); plat->set_ram_gov("performance"); }
        sys_write(p, "perf\n");
        break;
    case PROFILE_CUSTOM:
        sys_write(p, "custom\n");
        break;
    default:
        set_limits(cpu, gpu, ram);
        plat->set_cpu_gov("ondemand");
        plat->set_gpu_mode("auto");
        if (plat->perfmode_set) plat->perfmode_set("auto");
        else plat->set_ram_gov("ondemand");
        sys_write(p, "balanced\n");
        break;
    }
}
