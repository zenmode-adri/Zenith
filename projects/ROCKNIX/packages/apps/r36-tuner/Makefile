# SPDX-License-Identifier: MIT
CC ?= gcc
CFLAGS += -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDLIBS += -lSDL2 -lSDL2_ttf -lm -pthread
OBJ = main.o sys.o plat.o state.o i18n.o ui.o dtb.o bench.o tab_home.o tab_tune.o tab_advanced.o tab_tests.o

all: r36-tuner r36_cpubench r36_rambench r36_stress

r36-tuner: $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@ $(LDLIBS)

$(OBJ): tuner.h

r36_cpubench: r36_cpubench.c
	$(CC) -O2 $< -o $@
r36_rambench: r36_rambench.c
	$(CC) -O2 $< -o $@
r36_stress: r36_stress.c
	$(CC) -O2 $< -o $@ -lm -pthread

clean:
	rm -f *.o r36-tuner r36_cpubench r36_rambench r36_stress
