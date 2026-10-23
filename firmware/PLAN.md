# Firmware plan - STM32G474RE on the NUCLEO-G474RE

**Date:** 2026-10-15
**Status (2026-10-15, later the same day):** phases 0-3 are written and tested on the PC: see
[README.md](README.md) for what exists and [BRINGUP.md](BRINGUP.md) for the bench steps that remain. Decisions
taken (section 6): all three recommended options - CMake with STM32CubeCLT, the fit checked against Python
first, git in `firmware/` only (initialised; nothing committed yet). Not yet run on a Nucleo.

## 1. What the firmware does

The specification is the **Firmware timing table** in
[`acquisition_rev_C/README.md`](../schematic/comparator-path/acquisition_rev_C/README.md) (the pin table is just above
it). The current boards are rev E (the 80 x 60 stack on the rev E power board), and
[`acquisition_rev_E/README.md`](../schematic/comparator-path/acquisition_rev_E/README.md) keeps that table **without
its boot step "N5_SHDN HIGH"**: the power board's -5 V has no enable, so PB7 and bus pin 19 are spare. The K1 relay times are
in [`polarization_switch_rev_D/README.md`](../schematic/polarizaiton-switch/polarization_switch_rev_D/README.md)
("Firmware timing"); the rev E polarization board is rev D's circuit plus a 12 V input terminal. The chip and board
documents are in [`docs/`](docs/README.md). One shot, with every delay fixed from GATE_CMD falling (t = 0) and
**nothing timed on I_SENSE**:

1. **Boot:** measure VREFINT and the idle ADC code; ADC3 on PB0 at its longest sample time (640.5 cycles). **Idle:**
   GATE_CMD, RLY_CMD and TUNE low, BLANK2 high.
2. **Arm:** check (a), XFER_SENSE high -> RLY_CMD high -> check (a2) at +1 ms, XFER_SENSE up by >= 25 mV -> check (b)
   at +25 ms, XFER_SENSE low (K1 operated). Any failure: every line low, log it, no shot.
3. **Polarize:** GATE_CMD high >= 30 ms after RLY_CMD, for t_pol = 2-4 s. Log I_SENSE (1.000 V/A); abort above about
   1.6 V.
4. **Switch-off:** GATE_CMD low at 0; RLY_CMD low at +0.5 ms; TUNE high at +5 ms (tuned shots); ADC3 captures
   XFER_SENSE until +40 ms; check (c) at +38 / +39 / +40 ms and check (d) (the median rule) decide whether the
   transfer happened.
5. **Record:** BLANK2 low at +45 ms; discard until +60 ms; then ADC1 on PA0 at 20 kSPS for 0.94 s (18,800 samples)
   and TIM2_CH3 timestamps of both comparator edges on PB10, while XFER is watched (a drop voids the record).
6. **End:** TUNE low, BLANK2 high; fit the record; report. TUNE is never high at gate-off.

**The relay times since rev D.** The table was written for the rev C polarization board (G5LE relay). With the
G6S-2F of rev D and rev E, slowed by R3/C8, K1 makes about 7 ms after RLY_CMD, opens about 3.5 ms after gate-off, and the transfer
completes at about +11.6 ms instead of +18 ms. Read against the two READMEs, every check keeps its margin, so the
delays stay as written and only expected values move. The G6S's operate, release and bounce times are bench items
(rev D open item 3).

| Signal | MCU pin | Morpho | Bus | Use |
|---|---|---|---|---|
| ADC_PIN | PA0, ADC1_IN1 | CN7-28 | 9 (BP_OUT) | the record |
| I_SENSE | PA1, ADC1_IN2 | CN7-30 | 11 | coil current, 1.000 V/A |
| CMP_OUT | PB10, TIM2_CH3 | CN10-25 | - | Path A edges |
| XFER_SENSE | PB0, ADC3_IN12 (EXTI0) | CN7-34 | 13 (XFER), through R11 / R12 | transfer readback, 0.248 x XFER |
| TUNE | PC10 | CN7-1 | 15 | output |
| BLANK2 | PC11 | CN7-2 | 16 | output |
| GATE_CMD | PC12 | CN7-3 | 17 | output |
| RLY_CMD | PD2 | CN7-4 | 18 | output |
| (spare since rev E) | PB7 | CN7-21 | 19 | was N5_SHDN |
| HSE_IN | PF0 / OSC_IN | CN7-29 | - | TCXO, later |

One shot's data is about 60 KB of the 128 KB SRAM: 37.6 KB of samples, about 21 KB of edge times and 1.6 KB of XFER
capture.

## 2. How

- **The firmware does not wait for the boards.** Phases 0-3 run on a bare NUCLEO-G474RE with jumper wires; the boards
  (not ordered yet) come in at phase 4.
- **A portable core and a thin board layer.** The sequencer, the checks and the estimator are plain C with no HAL calls
  (`app/`), so they also build on the PC with the Cygwin gcc already installed (`C:\cygwin64\bin\gcc.exe`) and are
  tested there against the Python reference. Pins, clocks, ADC/DMA and timers live in `bsp/`: a pin change is one line.
- **Hardware keeps time.** The record's sample clock is a timer-triggered ADC with DMA, the edges are TIM2 captures with
  DMA, and the shot runs from a timer event list, never `HAL_Delay`. HAL sets the peripherals up; the timed paths use
  LL or registers.
- **One table of numbers.** Every delay, threshold and expected value lives in one header, each with its source, so a
  bench measurement changes one line.
- **Safe when anything goes wrong.** The boards' pull-downs hold every control line low in reset, and the firmware
  keeps that true:
  - outputs are written low before they are enabled;
  - the independent watchdog resets a hung MCU, which drops GATE_CMD and RLY_CMD together. The coil current is zero
    within about 70 us and K1 opens milliseconds later, so it still opens cold;
  - a separate top-priority timer ends the gate at a maximum on-time (proposed 4.5 s), whatever the sequencer does;
  - the ADC analog watchdog on I_SENSE turns the gate off from its interrupt;
  - no gate without checks (a) and (b), and the firmware stops after three missed transfers in a row.

**Tools already on the laptop** (checked 2026-10-15): STM32CubeCLT 1.21.0 (GNU tools for STM32, CMake, Ninja,
STM32CubeProgrammer, the ST-LINK GDB server), STM32CubeIDE 2.0.0, the STM32Cube_FW_G4_V1.6.3 package (NUCLEO-G474RE
templates and examples, HAL, LL, CMSIS-DSP), git 2.46 and Python with numpy. **Standalone STM32CubeMX is not
installed.**

## 3. Phases

### Phase 0 - set-up

| Step | Done when |
|---|---|
| Check every fact the code will hard-code in DS12288, RM0440 and UM2505 (now on disk): the ADC channels of PA0, PA1 and PB0, and whether PA1 also reaches ADC2 (which would leave ADC1 to the record alone); PB10 = TIM2_CH3; PB0's I/O type, injection limits and VIH / VIL (acquisition README open item 4); the ADC3 sample times; the UART and pins of the ST-LINK virtual COM port; the HSE fitted as delivered (UM2505 section 7.5.1) | a facts table in `firmware/README.md`, each line with its page |
| **Grace:** read the board's MB1367 revision off its sticker (C-04, C-05 or D-01; UM2505 section 10.1) | the matching schematic named in the facts table |
| The project (decision 1): SYSCLK 170 MHz from the HSE through the PLL, a build switch for the TCXO in bypass (HANDOFF items 25 and 27); folders `app/`, `bsp/`, `tests/`, `tools/`; version control (decision 3) | one command builds; STM32_Programmer_CLI flashes; LD2 blinks; a banner arrives on the virtual COM port; the host tests build with Cygwin gcc |

### Phase 1 - one peripheral at a time, on the bare Nucleo

| Peripheral | Bench test | Done when |
|---|---|---|
| The four outputs (GATE_CMD, RLY_CMD, TUNE, BLANK2) | logic analyzer from power-on | low from reset, no glitch when enabled |
| Clock: TIM2, 32-bit at 170 MHz | a timer output on a frequency counter | within the clock source's tolerance |
| Record: ADC1 on PA0, timer-triggered at 20 kSPS, DMA, 18,800 samples; VREFINT calibration (VREF+ is 3.25 V, HANDOFF item 26) | a sine from a signal generator (or the G474's DAC) into PA0, the record sent to the PC | the Python fit returns the generator's frequency; offset and noise logged |
| Edges: TIM2_CH3 on PB10, both edges, DMA | a square wave into PB10 | the period from the timestamps matches the generator |
| XFER_SENSE: ADC3 on PB0 at 640.5 cycles, DMA; its analog watchdog (or EXTI0 with the pin switched to a digital input) | a pot into PB0 | readings match a meter to a few mV; the trip fires |
| I_SENSE: PA1 with an analog watchdog at 1.6 V | a pot into PA1 with GATE_CMD high | GATE_CMD drops within one conversion of the crossing |
| Errata ES0430 sections 2.7.8, 2.7.9, 2.7.11 (ADC1 and ADC3 running together) and 2.1.2 | - | a workaround in the code, or a note saying why it does not apply |

Each result goes into `firmware/BRINGUP.md`.

### Phase 2 - the shot sequencer

- The state machine of section 1 on a timer event list, with checks (a), (a2), (b), (c), (d) and the record watch.
- A text command line on the virtual COM port (`shot <t_pol> [tuned]`, `status`, `abort`) and one log line per shot
  carrying every check's value and I_SENSE.
- Host tests: the state machine against synthetic XFER_SENSE and I_SENSE traces, for a healthy shot and for each fault
  the acquisition README lists (dead XFER, K1 not operating, no (a2) step, missed transfer, no K_XFER pickup,
  overcurrent, XFER dropping during the record), checked against its expected values - for example a (d) step of
  45-70 mV when healthy and about -4 to -2 mV with an open K_XFER coil.
- Bench: logic analyzer on the four outputs, with XFER emulated by a spare GPIO through a divider into PB0.
- **Done when** every edge lands within 0.1 ms of the table and each injected fault gives the right abort and log line.

### Phase 3 - data out and the frequency fit

- Each shot's record, edges, XFER capture and I_SENSE go to the PC as one binary block over the virtual COM port. A
  script in `tools/` saves it and fits it with the existing
  [`frequency_estimation.py`](../simulations/frequency-estimation/frequency_estimation.py) (an FFT seed, then a
  Levenberg-Marquardt fit of a damped sinusoid, which reaches the Cramer-Rao bound in simulation). pyserial is not
  confirmed installed; PowerShell's built-in serial port class can do the capture without an install.
- Then the fit on the MCU (decision 2): a seed from a short FFT (CMSIS-DSP) or from the comparator period, then
  Levenberg-Marquardt over the record with its sums in double. TIM2 timestamps stay integers: a float holds 170 MHz
  counts exactly for only 99 ms. Path A runs only under the acquisition README's rule: tuned shots, peak/rms >= 15,
  every edge gated +/-25 % against the fit's period, rising and falling estimates averaged.
- **Done when** the MCU fit and the Python fit agree within a tenth of the Cramer-Rao bound on the same records.

### Phase 4 - with the boards

During phases 0-3 the Nucleo runs from USB, as delivered. On the rev E carrier it runs from the carrier's 12 V -> 5 V
buck: **JP5 to E5V and J3's shunt fitted** (acquisition rev E README). In the order the boards arrive: the acquisition
carrier alone (the E5V supply, VREF+, the XFER readback), then the stack. Power a stack with XFER live only when every
board in it carries XFER on bus pin 13 (rev C or later), and only after J4 pin 13 is continuity-checked against pins
11, 12, 14 and 15 (acquisition rev E README open item 2). Then the bench list (HANDOFF item 2, acquisition rev C README
open item 5, rev D open item 3, rev E open item 5): K1 operate, release and bounce; the (a2) step; the K_XFER pickup;
the BLANK2 transient that sets the +60 ms record start; the buck's 500 kHz on ADC_PIN; the first FID untuned, then SW1
set from f_L, then tuned. Each measured number replaces its entry in the table.

## 4. Bench kit for phases 1-3

The NUCLEO-G474RE and its USB cable, a logic analyzer or a four-channel scope, a signal generator, a frequency counter
(or the scope's), two 10 k pots, a few resistors and jumper wires.

## 5. Left out on purpose

Low-power modes (ES0430 sections 2.2.5 and 2.2.10 matter only with the HSE in bypass), the internal COMP or VREFBUF in
place of the LT1719 or the Nucleo's reference, ADC oversampling, the ADS131M04 option, a display and local storage.

## 6. Decisions for Grace

| # | Question | Options | Recommended |
|---|---|---|---|
| 1 | Project set-up | **A** CMake with STM32CubeCLT, started from the CubeG4 1.6.3 NUCLEO-G474RE template; **B** pins and clocks generated from a CubeMX `.ioc` (graphical; standalone CubeMX is a download, rule 10) | **A**: nothing to install, and it builds and flashes from one command |
| 2 | Where the frequency fit runs first | **A** on the PC, then ported to the MCU; **B** on the MCU from the start | **A**: the Python fit exists and becomes the reference the C port is tested against |
| 3 | Version control | **A** git in `firmware/` only; **B** git for the whole project; **C** none | **A**: firmware changes in small steps and needs its history; the rest of the project stays as it is |
