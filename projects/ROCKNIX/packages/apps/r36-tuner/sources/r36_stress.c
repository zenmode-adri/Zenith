/* SPDX-License-Identifier: MIT
 * r36_stress <seconds>: stability test. One thread per core. Each one repeats work whose answer is known and counts every
 * wrong answer. An undervolted CPU or a memory too fast for its voltage usually gives wrong answers before it hangs.
 *   - integer: chains that must end in the same value every round
 *   - float:   a sum with a known result (uses the FPU/NEON)
 *   - memory:  writes patterns over 48 MB and reads them back, then copies between two blocks
 * It watches the temperature itself, so the limit holds even if the program that started it dies: at the limit everything waits
 * for the chip to cool down, and only the time under load counts. It gives up after 4 times the length of the test.
 * r36_stress <seconds> [--hot C] [--temp FILE] [--gpu 'command']   the command repeats as a graphics load and waits with the rest
 * Prints "S <seconds under load> <errors> <1 cooling>" every second and "RESULT <n> <done|hot|stopped>" at the end.
 * If nobody reads that any more it stops. Exit code: 0 no errors, 1 errors, 2 too hot to finish. */
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_MAX (48u * 1024 * 1024)
#define BLOCK_MIN (4u * 1024 * 1024)
static size_t BLOCK;   /* set from the free memory: being killed for lack of it would look like a failed test */

#define HOT_C 88
#define COOL_DOWN_C 10   /* starts again this far below the limit */

static volatile sig_atomic_t stop;
static volatile sig_atomic_t cooling;
static volatile unsigned long errors;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void err(unsigned long n) {
    if (!n) return;
    pthread_mutex_lock(&lock);
    errors += n;
    pthread_mutex_unlock(&lock);
}

static uint32_t int_round(void) {
    uint32_t a = 1, b = 2, c = 3, d = 4;
    for (uint32_t i = 0; i < 4000000u; i++) {
        a = a * 1664525u + 1013904223u;
        b = (b ^ a) * 22695477u + 1u;
        c = (c + b) * 1103515245u + 12345u;
        d = (d ^ (c >> 7)) + (a << 3);
    }
    return a ^ b ^ c ^ d;
}

static double float_round(void) {
    volatile double s = 0;
    for (int i = 1; i < 800000; i++) s += sqrt((double)i) * 1.0000001 / (double)i;
    return s;
}

static void *worker(void *arg) {
    (void)arg;
    uint32_t want_i = int_round();
    double want_f = float_round();
    uint32_t *a = malloc(BLOCK), *b = malloc(BLOCK);
    size_t words = BLOCK / sizeof(uint32_t);
    uint32_t round = 0;
    while (!stop) {
        while (cooling && !stop) usleep(200000);
        if (stop) break;
        if (int_round() != want_i) err(1);
        if (float_round() != want_f) err(1);
        if (a && b) {
            static const uint32_t fixed[3] = {0x55555555u, 0xAAAAAAAAu, 0x00FF00FFu};
            uint32_t p = round % 4 < 3 ? fixed[round % 4] : round * 2654435761u;
            for (size_t i = 0; i < words; i++) a[i] = p ^ (uint32_t)i;
#ifdef R36_STRESS_INJECT
            a[words / 2] ^= 1;   /* tests: a bit that memory got wrong */
#endif
            unsigned long bad = 0;
            for (size_t i = 0; i < words; i++) bad += a[i] != (p ^ (uint32_t)i);
            err(bad);
            memcpy(b, a, BLOCK);
            bad = 0;
            for (size_t i = 0; i < words; i++) bad += b[i] != (p ^ (uint32_t)i);
            err(bad);
        }
        round++;
    }
    free(a);
    free(b);
    return NULL;
}

static void on_term(int s) { (void)s; stop = 1; }

/* Half of the free memory, shared between two blocks per core */
static size_t pick_block(long cores) {
    long long avail_kb = 0;
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "MemAvailable: %lld", &avail_kb) == 1) break;
        fclose(f);
    }
    size_t b = avail_kb > 0 ? (size_t)(avail_kb * 1024 / 2 / (2 * cores)) : BLOCK_MIN;
    if (b > BLOCK_MAX) b = BLOCK_MAX;
    if (b < BLOCK_MIN) b = BLOCK_MIN;
    return b & ~(size_t)4095;
}

static int read_temp(const char *file) {
    int v = 0;
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return v / 1000;
}

/* In its own process group: stopping it stops whatever it runs. A shell that loses this program goes with it */
static pid_t start_gpu(const char *cmd) {
    char loop[1024];
    snprintf(loop, sizeof(loop), "while :; do ( %s ) >/dev/null 2>&1; sleep 1; done", cmd);
    pid_t p = fork();
    if (p == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        execl("/bin/sh", "sh", "-c", loop, (char *)NULL);
        _exit(127);
    }
    if (p > 0) setpgid(p, p);
    return p;
}

static void stop_gpu(pid_t p) {
    if (p <= 0) return;
    kill(-p, SIGTERM);
    kill(-p, SIGCONT);
    waitpid(p, NULL, 0);
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 60, hot_c = HOT_C;
    const char *temp_file = "/sys/class/thermal/thermal_zone0/temp", *gpu_cmd = NULL;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--hot")) hot_c = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--temp")) temp_file = argv[i + 1];
        else if (!strcmp(argv[i], "--gpu")) gpu_cmd = argv[i + 1];
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 1) cores = 1;
    if (cores > 16) cores = 16;
    BLOCK = pick_block(cores);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    pid_t gpu = gpu_cmd ? start_gpu(gpu_cmd) : -1;
    pthread_t th[16];
    for (long i = 0; i < cores; i++) pthread_create(&th[i], NULL, worker, NULL);

    int active = 0, too_hot = 0, stopped = 0;
    for (long wall = 0; active < seconds && !stop; wall++) {
        sleep(1);
        int t = read_temp(temp_file);
        if (!cooling && t >= hot_c) {
            cooling = 1;
            if (gpu > 0) kill(-gpu, SIGSTOP);
        } else if (cooling && t <= hot_c - COOL_DOWN_C) {
            cooling = 0;
            if (gpu > 0) kill(-gpu, SIGCONT);
        }
        if (!cooling) active++;
        if (printf("S %d %lu %d\n", active, (unsigned long)errors, (int)cooling) < 0) { stopped = 1; break; }   /* nobody is watching */
        if (wall >= 4L * seconds) { too_hot = 1; break; }
    }
    if (stop) stopped = 1;
    stop = 1;
    stop_gpu(gpu);
    for (long i = 0; i < cores; i++) pthread_join(th[i], NULL);
    printf("RESULT %lu %s\n", (unsigned long)errors, too_hot ? "hot" : stopped ? "stopped" : "done");
    return errors ? 1 : too_hot ? 2 : 0;
}
