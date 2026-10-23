# The console end to end on the PC: app.c with the fit and Path A, on the simulated board.
#   make -C tests/app run

CC ?= gcc
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wshadow -Wdouble-promotion -I../../app -I../shot
SRC = test_app.c ../shot/fake_hw.c ../../app/app.c ../../app/shot.c ../../app/xfer.c ../../app/xfer_model.c \
      ../../app/fmt.c ../../app/fit.c ../../app/noise.c ../../app/patha.c ../../app/gps.c

test_app: $(SRC) ../../app/*.h ../shot/fake_hw.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

run: test_app
	./test_app

clean:
	rm -f test_app test_app.exe

.PHONY: run clean
