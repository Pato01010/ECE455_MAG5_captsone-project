# The GPS module (app/gps.c) on the PC: sentences, pulses, UTC and the clock.
#   make -C tests/gps run

CC ?= gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wshadow -Wdouble-promotion -I../../app
SRC = test_gps.c ../../app/gps.c ../../app/fmt.c

test_gps: $(SRC) ../../app/gps.h ../../app/config.h ../../app/fmt.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

run: test_gps
	./test_gps

clean:
	rm -f test_gps test_gps.exe

.PHONY: run clean
