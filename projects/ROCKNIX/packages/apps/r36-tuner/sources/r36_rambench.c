/* SPDX-License-Identifier: MIT
 * r36_rambench: 128 MB memset (write) and memcpy (copy) bandwidth, BENCH_SECS
 * seconds each. Prints two lines: write MB/s, copy MB/s. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SZ (128 * 1024 * 1024)
#define BENCH_SECS 10

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
    char *a = malloc(SZ), *b = malloc(SZ);
    if (!a || !b) { puts("0\n0"); return 1; }
    memset(a, 0, SZ);
    memset(b, 0, SZ);

    int runs = 0;
    double t0 = now(), t1;
    do {
        memset(a, 0xAB, SZ);
        __asm__ volatile("" ::: "memory");
        runs++;
        t1 = now();
    } while (t1 - t0 < BENCH_SECS);
    double w = (double)runs * SZ / 1024.0 / 1024.0 / (t1 - t0);

    runs = 0;
    t0 = now();
    do {
        memcpy(b, a, SZ);
        __asm__ volatile("" ::: "memory");
        runs++;
        t1 = now();
    } while (t1 - t0 < BENCH_SECS);
    double c = (double)runs * SZ / 1024.0 / 1024.0 / (t1 - t0);

    printf("%.0f\n%.0f\n", w, c);
    return 0;
}
