# Shot sequencer tests on the PC (Cygwin gcc, or any gcc).
#   make -C tests/shot run

CC ?= gcc
CFLAGS = -std=c99 -O2 -g -Wall -Wextra -Wshadow -Wdouble-promotion -I../../app -I.
SRC = test_shot.c fake_hw.c ../../app/shot.c ../../app/xfer.c ../../app/xfer_model.c

test_shot: $(SRC) ../../app/*.h fake_hw.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

run: test_shot
	./test_shot

clean:
	rm -f test_shot test_shot.exe

.PHONY: run clean
