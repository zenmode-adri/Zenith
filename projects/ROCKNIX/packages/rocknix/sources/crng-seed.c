/* crng-seed: carga/guarda una semilla de entropia en /storage y la ACREDITA al crng del kernel.
 *   crng-seed load FICHERO   -> mezcla + acredita (RNDADDENTROPY) y renueva el fichero (nunca se reutiliza una semilla)
 *   crng-seed save FICHERO   -> escribe 512 B nuevos (getrandom bloqueante; usar solo con el crng ya listo)
 * Sale 0 siempre en "load" aunque no haya semilla (no debe bloquear ni romper el arranque). */
#include <fcntl.h>
#include <linux/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <unistd.h>

#define SEED 512

static int save(const char *path, unsigned flags) {
    unsigned char b[SEED];
    char tmp[4096];
    ssize_t n = getrandom(b, SEED, flags);
    if (n != SEED) return 1;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 1;
    if (write(fd, b, SEED) != SEED || fsync(fd) != 0) { close(fd); unlink(tmp); return 1; }
    close(fd);
    return rename(tmp, path) ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "save")) return save(argv[2], 0);
    if (strcmp(argv[1], "load")) return 2;

    struct { struct rand_pool_info i; unsigned char buf[SEED]; } p;
    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { puts("crng-seed: sin semilla"); return 0; }
    ssize_t n = read(fd, p.buf, SEED);
    close(fd);
    if (n < 32) { puts("crng-seed: semilla corta, ignorada"); return 0; }
    p.i.buf_size = n;
    p.i.entropy_count = n * 8;
    int r = open("/dev/urandom", O_RDWR);
    if (r < 0 || ioctl(r, RNDADDENTROPY, &p.i) != 0) { puts("crng-seed: fallo RNDADDENTROPY"); return 0; }
    close(r);
    printf("crng-seed: acreditados %zd bytes\n", n);
    save(argv[2], GRND_NONBLOCK);   /* renovar: la semilla usada no se reutiliza */
    return 0;
}
