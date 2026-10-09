# SPDX-License-Identifier: MIT

CC ?= gcc
CFLAGS += -O2 -Wall -Wno-unused-function -std=gnu11

all: r36-tuner r36_cpubench r36_rambench

r36-tuner: main.c
	$(CC) $(CFLAGS) main.c -o $@ -lSDL2 -lSDL2_ttf -lm -pthread

r36_cpubench: r36_cpubench.c
	$(CC) $(CFLAGS) r36_cpubench.c -o $@

r36_rambench: r36_rambench.c
	$(CC) $(CFLAGS) r36_rambench.c -o $@

clean:
	rm -f r36-tuner r36_cpubench r36_rambench
