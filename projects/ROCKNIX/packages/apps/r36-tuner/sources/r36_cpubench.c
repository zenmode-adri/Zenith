/* SPDX-License-Identifier: MIT
 * r36_cpubench: single-thread integer ALU benchmark. Four independent LCG
 * chains kept in registers, run for BENCH_SECS seconds. Prints the total
 * number of operations on one line (same scale as the original R36 Tuner
 * binary, so old and new scores are comparable). */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define BENCH_SECS 30
#define BLOCK_ITERS 10000000u

int main(void) {
    uint32_t a = 1, b = 2, c = 3, d = 4;
    long long iters = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        for (uint32_t i = 0; i < BLOCK_ITERS; i++) {
            a = a * 1664525u + 1013904223u;
            b = b * 1664525u + 1013904223u;
            c = c * 1664525u + 1013904223u;
            d = d * 1664525u + 1013904223u;
        }
        iters += 4LL * BLOCK_ITERS;
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while (t1.tv_sec - t0.tv_sec < BENCH_SECS);
    if ((a ^ b ^ c) == d) iters++;   /* keep the chains alive */
    printf("%lld\n", iters);
    return 0;
}
