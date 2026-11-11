/* SPDX-License-Identifier: MIT */
#include "tuner.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Fake root for the PC tests and previews: R36T_ROOT=/some/dir */
const char *rp(const char *path) {
    static char buf[4][400];
    static int slot;
    static const char *root;
    static int init;
    if (!init) { root = getenv("R36T_ROOT"); init = 1; }
    if (!root || !*root || path[0] != '/') return path;
    char *b = buf[slot = (slot + 1) & 3];
    snprintf(b, sizeof(buf[0]), "%s%s", root, path);
    return b;
}

int sys_read(const char *path, char *buf, int n) {
    buf[0] = 0;
    FILE *f = fopen(rp(path), "r");
    if (!f) return -1;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    while (r > 0 && strchr("\n\r \t", buf[r - 1])) buf[--r] = 0;
    return 0;
}

int sys_int(const char *path) {
    char b[64];
    return sys_read(path, b, sizeof(b)) ? 0 : atoi(b);
}

long long sys_ll(const char *path) {
    char b[64];
    return sys_read(path, b, sizeof(b)) ? 0 : atoll(b);
}

int sys_exists(const char *path) { return access(rp(path), F_OK) == 0; }

int sys_write(const char *path, const char *val) {
    FILE *f = fopen(rp(path), "w");
    if (f) {
        int bad = fputs(val, f) < 0;
        bad |= fclose(f) != 0;
        if (!bad) return 0;
    }
    return sys_root("echo '%s' | tee '%s' >/dev/null", val, rp(path));
}

static void cmd_log(const char *cmd) {
    if (!getenv("R36T_ROOT")) return;
    FILE *f = fopen(rp("/cmd.log"), "a");
    if (f) { fprintf(f, "%s\n", cmd); fclose(f); }
}

/* dArkOSRE runs as user ark, Zenith as root. R36T_SUDO: tests go through the sudo path on a fake console */
static int am_root(void) {
    if (getenv("R36T_SUDO")) return 0;
    return geteuid() == 0 || getenv("R36T_ROOT");
}

int sys_root(const char *fmt, ...) {
    char cmd[1600], out[3400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    cmd_log(cmd);
    if (am_root()) {
        snprintf(out, sizeof(out), "( %s ) >/dev/null 2>&1", cmd);
    } else {
        char esc[3000]; size_t j = 0;
        for (const char *p = cmd; *p && j < sizeof(esc) - 5; p++) {
            if (*p == '\'') { memcpy(esc + j, "'\\''", 4); j += 4; }
            else esc[j++] = *p;
        }
        esc[j] = 0;
        snprintf(out, sizeof(out), "echo ark | sudo -S sh -c '%s' >/dev/null 2>&1", esc);
    }
    int rc = system(out);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

int sys_cap(char *out, int n, const char *fmt, ...) {
    char cmd[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    out[0] = 0;
    cmd_log(cmd);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t r = fread(out, 1, n - 1, f);
    int rc = pclose(f);
    out[r] = 0;
    while (r > 0 && strchr("\n\r \t", out[r - 1])) out[--r] = 0;
    return rc == -1 ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : 1);
}

void *sys_popen_root(const char *cmd) {
    char out[2600];
    cmd_log(cmd);
    if (am_root()) return popen(cmd, "r");
    char esc[2400];
    size_t j = 0;
    for (const char *p = cmd; *p && j < sizeof(esc) - 5; p++) {
        if (*p == '\'') { memcpy(esc + j, "'\\''", 4); j += 4; }
        else esc[j++] = *p;
    }
    esc[j] = 0;
    snprintf(out, sizeof(out), "echo ark | sudo -S sh -c '%s' 2>/dev/null", esc);
    return popen(out, "r");
}

int sys_pclose(void *f) { return pclose((FILE *)f); }

int sys_split(char *text, char *tok[], int max) {
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(text, " \t\r\n", &save); t && n < max; t = strtok_r(NULL, " \t\r\n", &save))
        tok[n++] = t;
    return n;
}

int sys_has_word(const char *list, const char *word) {
    size_t w = strlen(word);
    for (const char *p = list; (p = strstr(p, word)); p += w)
        if ((p == list || strchr(" \t\n", p[-1])) && (p[w] == 0 || strchr(" \t\n", p[w]))) return 1;
    return 0;
}

void sys_mkdirs(const char *path) {
    char b[400];
    snprintf(b, sizeof(b), "%s", rp(path));
    for (char *p = b + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(b, 0755); *p = '/'; }
    mkdir(b, 0755);
}
