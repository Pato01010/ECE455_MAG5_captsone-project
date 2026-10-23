# Formatting / parsing / CRC tests on the PC.
#   make -C tests/fmt run

CC ?= gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wshadow -Wdouble-promotion -I../../app

test_fmt: test_fmt.c ../../app/fmt.c ../../app/fmt.h
	$(CC) $(CFLAGS) -o $@ test_fmt.c ../../app/fmt.c -lm

run: test_fmt
	./test_fmt

clean:
	rm -f test_fmt test_fmt.exe

.PHONY: run clean
