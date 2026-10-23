# The per-shot uncertainty (app/noise.c) against the real scatter of f, on the PC.
#   make -C tests/noise run

CC ?= gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wshadow -Wdouble-promotion -I../../app
SRC = test_noise.c ../../app/noise.c ../../app/fit.c

test_noise: $(SRC) ../../app/noise.h ../../app/fit.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

run: test_noise
	./test_noise

clean:
	rm -f test_noise test_noise.exe

.PHONY: run clean
