/* SPDX-License-Identifier: MIT
 * r36_cpubench: single-thread integer ALU benchmark. Four independent LCG
 * chains (same loop as quick_lcg_bench in the tuner UI) for BENCH_SECS seconds.
 * Prints the total number of operations on one line. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define BENCH_SECS 30

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
    volatile uint32_t a = 1, b = 2, c = 3, d = 4;
    long long iters = 0;
    double t0 = now();
    while (now() - t0 < BENCH_SECS) {
        for (int i = 0; i < 1000000; i++) {
            a = a * 1664525u + 1013904223u;
            b = b * 1664525u + 1013904223u;
            c = c * 1664525u + 1013904223u;
            d = d * 1664525u + 1013904223u;
        }
        iters += 4000000;
    }
    if ((a ^ b ^ c ^ d) == 0) iters++;
    printf("%lld\n", iters);
    return 0;
}
