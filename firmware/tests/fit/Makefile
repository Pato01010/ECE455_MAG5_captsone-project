# Host test of fit.c / patha.c (Cygwin gcc) and their Cortex-M4F compile check.
#   make          out/fit_test.exe, plus fit.c and patha.c compiled for the M4F (out/arm/*.o, *.lst);
#                 every compile must be warning-free, or make stops
#   make run      the whole comparison against the Python reference (py -3.14 run_fit_tests.py)
#   make clean    removes out/ (everything in it is generated)

CC          = gcc
ARM_BIN     = C:/ST/STM32CubeCLT_1.21.0/GNU-tools-for-STM32/bin
ARM_CC      = $(ARM_BIN)/arm-none-eabi-gcc.exe
ARM_OBJDUMP = $(ARM_BIN)/arm-none-eabi-objdump.exe
PYTHON      = py -3.14
APP         = ../../app
OUT         = out
WARN        = -std=c99 -O2 -Wall -Wextra -Wdouble-promotion -Wshadow
ARM_FLAGS   = $(WARN) -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -c
HDRS        = $(APP)/fit.h $(APP)/patha.h

# $(call strict,<compile command>,<object>): run it, show its diagnostics, fail if there were any.
# .DELETE_ON_ERROR removes an object that compiled with warnings, so the next make checks it again.
strict = $(1) > $(2).log 2>&1; s=$$?; cat $(2).log; test $$s -eq 0 && test ! -s $(2).log
.DELETE_ON_ERROR:

all: $(OUT)/fit_test.exe $(OUT)/arm/fit.o $(OUT)/arm/patha.o

$(OUT)/host $(OUT)/arm:
	mkdir -p $@

$(OUT)/host/fit_test.o: fit_test.c $(HDRS) | $(OUT)/host
	$(call strict,$(CC) $(WARN) -I$(APP) -c $< -o $@,$@)

$(OUT)/host/%.o: $(APP)/%.c $(HDRS) | $(OUT)/host
	$(call strict,$(CC) $(WARN) -I$(APP) -c $< -o $@,$@)

$(OUT)/fit_test.exe: $(OUT)/host/fit_test.o $(OUT)/host/fit.o $(OUT)/host/patha.o
	$(CC) -o $@ $^ -lm

$(OUT)/arm/%.o: $(APP)/%.c $(HDRS) | $(OUT)/arm
	$(call strict,$(ARM_CC) $(ARM_FLAGS) $< -o $@,$@)
	$(ARM_OBJDUMP) -d $@ > $(OUT)/arm/$*.lst

run: all
	$(PYTHON) run_fit_tests.py --no-build

clean:
	rm -rf $(OUT)

.PHONY: all run clean
