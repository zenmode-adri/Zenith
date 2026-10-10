/* teclado virtual para probar el tuner: lee lineas de un FIFO: up down left right enter back esc | sleep MS | quit */
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd;

static void emit(int type, int code, int val) {
    struct input_event e; memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = val;
    if (write(fd, &e, sizeof e) < 0) perror("write");
}
static void press(int key) {
    emit(EV_KEY, key, 1); emit(EV_SYN, SYN_REPORT, 0);
    usleep(60000);
    emit(EV_KEY, key, 0); emit(EV_SYN, SYN_REPORT, 0);
    usleep(120000);
}

int main(int argc, char **argv) {
    const char *fifo = argc > 1 ? argv[1] : "/tmp/tuner/kbd.fifo";
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    int keys[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_BACKSPACE, KEY_ESC, KEY_TAB, KEY_X, KEY_Z, KEY_S, KEY_A, KEY_Q, KEY_W };
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    struct uinput_setup us; memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB; us.id.vendor = 0x1234; us.id.product = 0x5678;
    strcpy(us.name, "r36-test-keyboard");
    ioctl(fd, UI_DEV_SETUP, &us);
    ioctl(fd, UI_DEV_CREATE);
    sleep(2);
    unlink(fifo);
    mkfifo(fifo, 0666);
    printf("listo\n"); fflush(stdout);
    for (;;) {
        FILE *f = fopen(fifo, "r");
        if (!f) break;
        char ln[64];
        while (fgets(ln, sizeof ln, f)) {
            ln[strcspn(ln, "\n")] = 0;
            if (!strcmp(ln, "up")) press(KEY_UP);
            else if (!strcmp(ln, "down")) press(KEY_DOWN);
            else if (!strcmp(ln, "left")) press(KEY_LEFT);
            else if (!strcmp(ln, "right")) press(KEY_RIGHT);
            else if (!strcmp(ln, "enter")) press(KEY_ENTER);
            else if (!strcmp(ln, "back")) press(KEY_BACKSPACE);
            else if (!strcmp(ln, "esc")) press(KEY_ESC);
            else if (!strcmp(ln, "A")) press(KEY_X);
            else if (!strcmp(ln, "B")) press(KEY_Z);
            else if (!strcmp(ln, "X")) press(KEY_S);
            else if (!strcmp(ln, "Y")) press(KEY_A);
            else if (!strcmp(ln, "L")) press(KEY_Q);
            else if (!strcmp(ln, "R")) press(KEY_W);
            else if (!strcmp(ln, "hold tab")) { emit(EV_KEY, KEY_TAB, 1); emit(EV_SYN, SYN_REPORT, 0); }
            else if (!strcmp(ln, "release tab")) { emit(EV_KEY, KEY_TAB, 0); emit(EV_SYN, SYN_REPORT, 0); }
            else if (!strncmp(ln, "sleep ", 6)) usleep(atoi(ln + 6) * 1000);
            else if (!strcmp(ln, "quit")) { ioctl(fd, UI_DEV_DESTROY); close(fd); unlink(fifo); return 0; }
        }
        fclose(f);
    }
    return 0;
}
