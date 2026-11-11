/* SPDX-License-Identifier: MIT */
#include "tuner.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

State st;

/* First power supply of type Battery; the name changes between kernels */
static const char *battery_dir(void) {
    static char dir[200];
    static int done;
    if (done) return dir;
    done = 1;
    DIR *d = opendir(rp("/sys/class/power_supply"));
    if (!d) return dir;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300], t[32];
        snprintf(p, sizeof(p), "/sys/class/power_supply/%s/type", e->d_name);
        if (sys_read(p, t, sizeof(t)) == 0 && strcmp(t, "Battery") == 0) {
            snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", e->d_name);
            break;
        }
    }
    closedir(d);
    return dir;
}

static void read_battery(void) {
    const char *dir = battery_dir();
    st.bat_pct = -1;
    st.bat_w = -1;
    st.bat_charging = 0;
    if (!dir[0]) return;
    char p[300], b[32];
    snprintf(p, sizeof(p), "%s/capacity", dir);
    if (sys_read(p, b, sizeof(b)) == 0) st.bat_pct = atoi(b);
    snprintf(p, sizeof(p), "%s/status", dir);
    if (sys_read(p, b, sizeof(b)) == 0) st.bat_charging = strcmp(b, "Charging") == 0 || strcmp(b, "Full") == 0;
    snprintf(p, sizeof(p), "%s/voltage_now", dir);
    long long uv = sys_exists(p) ? sys_ll(p) : 0;
    snprintf(p, sizeof(p), "%s/current_now", dir);
    long long ua = sys_exists(p) ? sys_ll(p) : 0;
    if (uv > 0 && ua != 0) st.bat_w = (float)((double)uv / 1e6 * llabs(ua) / 1e6);
}

static void read_driver(void) {
    char p[300];
    ssize_t n = readlink(rp("/sys/devices/platform/ff400000.gpu/driver"), p, sizeof(p) - 1);
    st.gpu_driver[0] = 0;
    if (n <= 0) return;
    p[n] = 0;
    const char *b = strrchr(p, '/');
    b = b ? b + 1 : p;
    snprintf(st.gpu_driver, sizeof(st.gpu_driver), "%s", strncmp(b, "mali", 4) == 0 ? "libmali" : b);
}

static void push(float *h, float v) {
    if (st.h_n < HIST_N) h[st.h_n] = v;
    else { memmove(h, h + 1, sizeof(float) * (HIST_N - 1)); h[HIST_N - 1] = v; }
}

/* Previews only: a plausible past, so the graphs can be judged */
static void seed_history(void) {
    for (int i = 0; i < HIST_N; i++) {
        float f = (float)((i * 37) % 23) / 23.0f, g = (float)i / HIST_N;
        st.h_temp[i] = 44 + 8 * g + f;
        st.h_cpu[i] = 1008 + 288 * f;
        st.h_gpu[i] = 200 + 400 * ((i * 13) % 17 > 8 ? f : 0);
        st.h_ram[i] = 328 + 458 * ((i * 7) % 11 > 6 ? 1 : 0.2f);
    }
    st.h_n = HIST_N;
}

void state_refresh(void) {
    if (!st.h_n && getenv("R36T_SEED")) seed_history();
    st.cpu_mhz = sys_int(CPU_POLICY "/scaling_cur_freq") / 1000;
    st.cpu_max_mhz = sys_int(CPU_POLICY "/scaling_max_freq") / 1000;
    st.gpu_mhz = (int)(sys_ll(GPU_DEVFREQ "/cur_freq") / 1000000);
    st.gpu_max_mhz = (int)(sys_ll(GPU_DEVFREQ "/max_freq") / 1000000);
    st.ram_mhz = (int)(sys_ll(DMC_DEVFREQ "/cur_freq") / 1000000);
    st.ram_max_mhz = (int)(sys_ll(DMC_DEVFREQ "/max_freq") / 1000000);
    st.temp = sys_int(CPU_TEMP) / 1000.0f;
    sys_read(CPU_POLICY "/scaling_governor", st.cpu_gov, sizeof(st.cpu_gov));
    sys_read(GPU_DEVFREQ "/governor", st.gpu_gov, sizeof(st.gpu_gov));
    sys_read(DMC_DEVFREQ "/governor", st.ram_gov, sizeof(st.ram_gov));
    st.boost = sys_exists(BOOST_FILE) ? sys_int(BOOST_FILE) : -1;
    read_battery();
    read_driver();
    push(st.h_temp, st.temp);
    push(st.h_cpu, (float)st.cpu_mhz);
    push(st.h_gpu, (float)st.gpu_mhz);
    push(st.h_ram, (float)st.ram_mhz);
    push(st.h_bat, st.bat_w);
    if (st.h_n < HIST_N) st.h_n++;
}
