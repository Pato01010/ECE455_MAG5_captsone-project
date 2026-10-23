# Magnetometer firmware - STM32G474RE on the NUCLEO-G474RE

**Version:** 0.2.0 (2026-10-22: the GPS; 0.1.0 was 2026-10-15)
**State:** written, builds clean (no warnings), tested on the PC; **not yet run on a Nucleo** (none was connected),
and the GPS not yet on the bench.
The bench steps are in [BRINGUP.md](BRINGUP.md); the plan this follows is [PLAN.md](PLAN.md).

The firmware runs one shot of the instrument from a command:
1. Arm: checks (a), (a2) and (b) of the XFER readback.
2. Polarize, watching I_SENSE.
3. Switch off: checks (c) and (d).
4. Record 0.94 s of the free-induction decay at 20 kSPS together with the comparator edges.
5. Report:
   - the frequency, fitted at the Cramer-Rao bound;
   - the field in nT;
   - Path A, the comparator estimate;
   - every check's value;
   - the measured time of every step;
   - with the GPS connected: the UTC of the record, the position, and the frequencies corrected by the
     clock rate the GPS's 1 PPS measures (section "GPS").

The specification is the "Firmware timing table" in
[acquisition_rev_C/README.md](../schematic/comparator-path/acquisition_rev_C/README.md). The rev E boards
([acquisition_rev_E/README.md](../schematic/comparator-path/acquisition_rev_E/README.md)) keep that table
without the boot step "N5_SHDN HIGH". Every number the code uses is in [app/config.h](app/config.h), with its
source.

## Quick start

```bash
powershell -ExecutionPolicy Bypass -File build.ps1
```
```bash
powershell -ExecutionPolicy Bypass -File flash.ps1
```
```bash
py -3.14 tools/mag.py term
```

`build.ps1` uses the CMake, Ninja and GNU tools inside STM32CubeCLT 1.21.0; nothing is installed. The output is
`build/release/magnetometer.elf` (also `.hex` and `.bin`). Other presets: `build.ps1 debug`, and
`build.ps1 tcxo` (the TCXO in HSE bypass). `flash.ps1` programs the board over its ST-LINK with
STM32_Programmer_CLI and resets it.

The console is LPUART1 on the ST-LINK virtual COM port, **115200 baud**, 8N1, lines ending in CR or LF.
At reset the board prints:

```
MAGNETOMETER fw=0.2.0 clock=HSE_crystal vref_mV=3260 reset=power-on silicon=X watchdog=runs_when_halted
```

- **`clock`** must read `HSE_crystal` (or `HSE_TCXO`). `HSI_1pct` means the HSE did not start, and the field would
  then be wrong by up to 1 % (500 nT).
- **`silicon`** is the chip revision from DBGMCU_IDCODE. It says which ES0430 errata apply; `not_in_ES0430_rev9`
  means none of the errata sheet's columns covers this chip.
- **`vref_mV`** outside 3000-3500 is refused with a warning, and the nominal 3250 mV is used instead: every voltage
  the firmware reads depends on it, the I_SENSE trip point included.
- **`watchdog=runs_when_halted`**: a release build. A debugger halt resets the board 0.5 s later, gate off.
  `frozen_when_halted` is a debug build, so stepping works. **Never halt or step a debug build with the 12 V supply
  on:** the gate would stay on for as long as the core is halted.

## Commands

| Command | Does |
|---|---|
| `shot <t_pol_s> [tuned] [dump]` | One shot. t_pol 0.05-4 s (the specification is 2-4 s). `tuned` raises TUNE from +5 ms to the end of the record. `dump` sends the raw record after the report |
| `dry <t_pol_s> [tuned] [dump]` | The whole sequence with every control line left idle. The checks are measured and logged, not enforced. Safe on a full stack |
| `run <n> <t_pol_s> [tuned] [dump] [gap=<s>]` | n shots, 1 s apart by default (gap 0-3600 s). Stops after 3 missed transfers in a row (K1's NC contact) or an abort. While a shot or a run is in progress, gaps included, `shot`, `dry` and `run` are refused: `abort` first |
| `abort` (or the B1 button) | Stops at once, keeping the gate-off -> RLY_CMD order |
| `emu <mode>` | Bench only: DAC1 on PA4 plays XFER_SENSE. Jumper PA4 to PB0 on a bare Nucleo. Modes: `none`, `dead`, `no_release`, `k1_stuck`, `missed`, `no_pickup`, `drop`, `rev_c`, `off` |
| `selftest` | The on-board fit against three synthetic records: f error and time |
| `gps` | One `GPS` line: the current UTC and position fields, the clock measurement (`clk_ppm`, `clk_s`, `clk_age_s`), the NMEA-after-PPS delay (`lag_ms`), and the counters (`pps`, `labels`, `rmc`, `gga`, `bad_sum`, `pps_bad`, `pps_gaps`, `pps_missed`, `pps_lost`, `utc_jumps`, `dropped` ...) |
| `gps raw` | Echoes the next 20 RMC/GGA sentences as `NMEA <sentence>` lines (other sentences are dropped in the interrupt and never reach it) |
| `status`, `config`, `vref`, `help` | State; the timing constants; re-measure VREF+; the list |

**Never use `emu`, `shot` or `run` with a stack you are not ready to polarize.** `dry` is the command for checking a
stack without moving anything.

## What a shot reports

One line per kind, all `key=value`, so [tools/mag.py](tools/mag.py) or a terminal can read them. Every shot
ends with `DONE id=<n>`.

| Line | Content |
|---|---|
| `SHOT` | `result`:<br>- `ok`: a usable record<br>- `invalid`: the record exists but a check failed<br>- `aborted`: stopped before or during the record<br>- `no_fid`: every check passed but the fit found no decay above the noise; this is the only way a welded or dirty K_XFER contact shows<br>`reason`: the first problem found<br>`clock`: the time-base source<br>GPS fields (`na` when unknown; section "GPS"): `utc_at` (`rec0` = record sample 0, else `gate_off` or `arm`), `utc`, `fix` (0 none, 1 GPS, 2 differential), `sats`, `hdop`, `lat`, `lon` (degrees, + north / east), `alt_m`, `spd_mps`, `cog_deg`, `pos_age_s` (how much older the position is than `utc`), `clk_ppm` (the measured clock's error; `na` = no correction applied) and `clk_s` (seconds of pulses it spans) |
| `CHECK` | (a) `a_mV`, (a2) `a2_mV`, (b) `b_mV`, I_SENSE at half and near the end of the polarization (`i_mid_mV`, `i_end_mV`; 1.000 V/A), (c) `c1..c3_mV`, (d) `d_rise_ms d_step_mV d_pick_ms d_flag`, and `watch` (XFER fell during the record). A check or reading that never ran (the shot stopped first) shows `na` |
| `TIME` | The measured time of every event in microseconds, from gate-off; from RLY_CMD if the gate never came on |
| `FIT` | `f_Hz`, `fsig_Hz`, `B_nT`, `Bsig_nT` (corrected by the GPS clock when `corr=gps`; `f_raw_Hz` is the same fit on the nominal 170 MHz, `corr=none` makes them equal), `A_mV`, `T2_s`, `noise_mV`, `snr` (amplitude at the record start / noise rms), `iters`, `ms` (fit time).<br>**`fsig_Hz` is the honest one-sigma error:** the fit's white-noise sigma (`fsig_white_Hz`) scaled by the square root of `noise_density`, the residual's measured noise density near f over a white density of the same variance (`app/noise.c`). For the chain's band-limited noise the white sigma alone is about 2x too small; the corrected one matches the real scatter within a few percent (`tests/noise`) |
| `PATHA` | Run only on tuned shots with snr >= 15 (the README rule). Gives `f_Hz` (corrected like FIT) and the mean of the rising and falling estimates against the fit, as `minus_fit_nT`. Otherwise `skipped=<why>`.<br>**Path A reads low by a fixed amount:** the comparator's hysteresis delays every crossing more as the decay shrinks. That is -0.4 to -1.3 mHz (0.01-0.03 nT) at peak/rms 19-55 with T2* 1 s, and -8.9 mHz at T2* 0.3 s, with only 4 codes of hysteresis. The board's +7 / -17 codes will read lower |
| `DUMP` ... `END` | With `dump`. A header line (counts, CRC-32, times), then the raw little-endian arrays: the record (uint16), the edges (uint32 TIM2 counts), XFER_SENSE before and after the gate (uint16). `mag.py` saves it to `shots/*.npz` and refits it with the project's Python estimator |

## GPS

A SparkFun GNSS L1/L5 Breakout NEO-F10N (GPS-24114, Digi-Key 1568-24114-ND; HANDOFF item 72) on the Nucleo's own
Arduino sockets. The Nucleo is the top of the stack, so **no board changes**: five jumper wires.

| NEO-F10N pin | Nucleo socket | STM32 | Notes |
|---|---|---|---|
| 5V | CN6-5 (5V) | - | the breakout's own AP2112K makes its 3.3 V: the Nucleo's 3V3 (the carrier's 3V3D) carries none of it |
| GND | CN6-6 | - | |
| TX | D0 (CN9-1) | PC5, USART1_RX, AF7 | 38400 baud 8N1, the NEO-F10N's default (`GPS_BAUD`). USART1 on D0/D1 is the Nucleo's default (SB13/SB19) |
| PPS | D3 (CN9-4) | PB3, TIM2_CH2, AF1 | rising edge at the top of each UTC second, only once the receiver has a fix |
| RX | D1 (CN9-2), optional | PC4, USART1_TX | **not driven**: the firmware never sends. Build with `-DMAG_GPS_TX=ON` to drive it |

**Before wiring it:** set the receiver up once on a laptop over its USB-C in u-center (SparkFun's hookup guide):
**leave only RMC and GGA on UART1 at 1 Hz** (recommended: the RMC then follows its pulse by about 0.1 s, well inside
the 0.7 s the firmware accepts; with every default sentence on, check `lag_ms`), optionally enable L5 with the
health override, and save to flash. Then **cut the breakout's USB-TX and USB-RX jumpers**, as SparkFun says to before wiring the UART
to a microcontroller: its CH340C runs from USB power and loads the lines when unplugged. Re-bridge them to use
u-center again. The antenna is any active SMA GNSS antenna (L1/L5 for L5). **Keep a magnet-base antenna 10 m or
more from the sensor head** (a ~1 A m2 base magnet: ~200 nT at 1 m, ~0.2 nT at 10 m).

How it works (`app/gps.c`):
- **Sentences:** the USART1 interrupt (priority 3, like the console) assembles sentences and queues RMC and GGA,
  each with the TIM2 count of its `$`. Other sentences are counted (`skipped`) and dropped.
- **Pulses:** TIM2 channel 2 captures each PPS rising edge in the same 170 MHz count as the comparator edges and
  the sequencer. The TIM2 interrupt (priority 1) only queues the count, then runs the sequencer as before.
- **The main loop** drains both queues in time order before each `app_poll`. Pulses whose spacing is a whole number
  of seconds (within 1000 ppm of nominal, then within 20 ppm of the clock the chain has measured itself) form a
  chain; a missed pulse (up to 20 s) is bridged. Any other edge (a glitch on the wire) is counted in `pps_bad` and
  ignored, so it moves neither the clock nor the UTC; three in a row, or a gap over 20 s, start a new chain, checked
  against nominal again until it has measured the clock (an old measurement can never lock the pulses out). The
  capture filter also rejects spikes shorter than 1.5 us. **The clock** is the count difference over the last 17 pulses divided by the seconds
  between them: TIM2 counts per true second. It is applied from 4 s of pulses on, and kept 300 s after the pulses
  stop.
- **UTC:** an RMC with status A and a whole second, arriving within 0.7 s after a pulse, names that pulse's second
  (a u-blox sends an epoch's sentences after its pulse). Two such RMCs on different pulses must agree before there
  is a UTC, and one that disagrees removes it until two agree again (`utc_jumps`): a late or stray sentence costs a
  second of `utc=na`, never a wrong time. Later pulses are counted from it, and any time within 10 s of the last
  pulse gets its UTC from the measured clock (the fixed ~1.5 us filter delay is not removed).
- **The correction:** the record (TIM6 off the same clock) and the edges (TIM2) are both timed by the HSE, so every
  frequency scales by k = ticks_per_s / 170e6: `f_Hz = f_raw_Hz x k`. 1 ppm is 0.05 nT at 50 uT; the pulses measure
  the clock to about 0.003 ppm over 16 s (u-blox time pulse 30 ns rms). This also means Y1 needs only to be stable
  for seconds, not accurate (HANDOFF item 25).
- **Positions** come from the latest RMC (status A) or GGA (quality > 0) and are reported for 5 s. They are the
  antenna's position, not the head's: correct for the tow cable (layback) afterwards, with `cog_deg`.
- **With nothing connected** PC5 is pulled up and PB3 pulled down: every GPS field reads `na`, `fix=0`, and shots run
  exactly as in 0.1.0.

**A survey log:** `py -3.14 tools/mag.py cmd "run 100 3 tuned gap=0" --csv survey.csv` appends one row per shot:
UTC, position, fix, result, B_nT, Bsig_nT, f_Hz, f_raw_Hz, clk_ppm, Path A.

`B_nT` uses the shielded-proton gyromagnetic ratio for water, 42.57638474 Hz/uT (CODATA 2018). **The READMEs'
42.5775 Hz/uT is the free-proton value, 26 ppm higher: about 1.3 nT at 50 uT.**

## How it is built

```
app/   portable C: the sequencer (shot.c), the XFER checks (xfer.c), the fit (fit.c), Path A (patha.c),
       the console and reports (app.c), formatting (fmt.c), the XFER waveform model (xfer_model.c),
       the GPS: NMEA, PPS, UTC and the clock measurement (gps.c).
       No register access: the PC tests compile it unchanged.
bsp/   the board: clocks, pins, console, watchdog (board.c); ADC/DMA/timers/interrupts behind app/hw.h (hw.c);
       the bench emulator (emu.c); start-up (main.c, startup_stm32g474xx.s); the linker script.
vendor/ ST's CMSIS, device headers and LL headers from STM32Cube_FW_G4_V1.6.3, with their licences and copyright
       notices; the one change is that their "@author" lines are removed (2026-10-23).
tests/ shot/ (the sequencer on a simulated board), app/, fmt/, noise/, gps/, fit/ (the fit and Path A against the
       Python reference), tools/.
tools/ mag.py, the PC side.
```

Peripherals:

| What | How |
|---|---|
| Time base | TIM2, 32-bit, free-running at 170 MHz (wraps every 25.3 s; all times are compared by signed difference). Compare channel 1 wakes the sequencer |
| Sequencer | `shot_tick()` from the TIM2 compare interrupt. Every step is due at a fixed time from RLY_CMD rising or from gate-off. Nothing is timed on I_SENSE, and nothing uses `HAL_Delay` |
| Record | ADC1 on PA0, triggered by TIM6 TRGO at 20 kHz, DMA into 18,800 samples |
| Comparator edges | TIM2_CH3 on PB10, both edges, DMA into up to 6,144 timestamps |
| XFER_SENSE | ADC3 on PB0 at 20 kHz (640.5 cycles: a 24.8 kohm source), DMA. From check (c) to the end of the record, PB0 is a digital input with an EXTI0 falling-edge watch |
| I_SENSE | ADC2 on PA1, continuous while the gate is on. Its analog watchdog (4 consecutive conversions above 1.6 V) drops GATE_CMD from its interrupt |
| Gate guard | TIM7 one-pulse: GATE_CMD drops at t_pol + 20 ms (never more than 4.5 s) whatever the sequencer does |
| Console | LPUART1 (PA2/PA3, AF12), receive by interrupt into a ring buffer, transmit polled |
| GPS | USART1 receive on PC5 (AF7, 38400 baud), by interrupt into `gps_rx_isr`; the PPS on PB3 = TIM2_CH2 (AF1), rising edge, always captured, into `gps_pps_isr`; `gps_poll` in the main loop |
| Watchdog | IWDG, about 0.5 s, frozen while a debugger halts the core |
| Clock | 24 MHz HSE (the Nucleo's crystal X3, or a TCXO in bypass), PLL x85/6/2 = 170 MHz, range 1 boost, 4 wait states. HSI16 if the HSE does not start. The clock security system catches an HSE that stops later |

Interrupt priorities: 0 for the analog watchdog and the gate guard (they drop GATE_CMD), 1 for the sequencer and
the XFER watch (the PPS capture shares the TIM2 vector: a queue write before `shot_tick`), 3 for the console and the
GPS receive. Only one ADC converts at a time.

Memory: 66 KB of 512 KB flash (57 KB before the GPS). 66 KB of SRAM1/SRAM2 for every DMA buffer, and about 3 KB for
the GPS queues and state (68.8 KB of 96 KB in all) (record 37.6 KB, edges 24.6 KB, XFER
2.9 KB, I_SENSE 128 B). CCM SRAM holds only the stack (8 KB) and the fit's workspace (8 KB): no DMA can reach it at
0x10000000. Stack use, from the compiler's `-fstack-usage`: the deepest chain (main loop -> post-processing ->
fit) is about 2.9 KB, plus about 1 KB for three nested interrupt levels with FPU frames. That is about 4 KB of 8 KB.

## Safety design

The boards' pull-downs hold every control line low in reset. The firmware keeps it that way:
- **At start-up:** every control line is written low before it becomes an output. GPIO comes first in `main()`,
  before the clock.
- **GATE_CMD high:** happens in exactly one place, after checks (a), (a2) and (b) pass. TUNE is forced low before
  every GATE_CMD edge.
- **K1 opens cold:** RLY_CMD falls only 0.5 ms after GATE_CMD fell, on every path: normal, user abort, overcurrent,
  guard, clock failure, any other NMI, and hard fault. The coil current is zero 59-71 us after gate-off.
- **Hardware backstops:** the analog watchdog on I_SENSE and the TIM7 guard drop GATE_CMD in their own interrupts,
  above the sequencer. An abort drops the gate before it stops anything else.
- **Hang or fault:**
  - the watchdog resets a hung MCU, and the pull-downs then drop every line together, which is safe: K1 takes
    milliseconds to open;
  - a hard fault drops GATE_CMD and TUNE without touching the stack (the fault may have come from a broken
    stack), waits about 1 ms, drops RLY_CMD, then waits for the watchdog;
  - an HSE failure (NMI) aborts the shot and reports it, and no shot starts until the clock has been
    re-initialised;
  - any other NMI (flash ECC, SRAM parity) drops the gate at once, then RLY_CMD, then waits for the watchdog.
- **A debugger halt:** in a release build the watchdog keeps running while the core is halted, so a breakpoint
  during polarization resets the board 0.5 s later. A debug build freezes it so stepping works, and says so in its
  banner. **Never halt or step a debug build with the 12 V supply on.** A gate on-time limit that does not depend
  on the CPU at all would need hardware (open item 7).
- **Stopping a run:** no gate without (a) and (b); a run stops after three missed transfers in a row.

Tests: `tests/shot` runs 19 scenarios, healthy and every fault (clock failure included), against the table with
these invariants checked on each. Ten deliberately broken versions of the sequencer and the console were each
caught by the tests. An independent review of the safety paths, the timing and the register code (2026-10-15)
found no way for the gate to come on early. Its other findings are fixed:
- the debugger-halt policy above;
- the NMI and HardFault order;
- `run` gaps above 12.6 s, which wrapped;
- commands typed during a run;
- the ADC register-write rules;
- the reports of aborted shots.

## Errata handled (ES0430 rev 9)

| Erratum | What the firmware does |
|---|---|
| 2.1.2 VDIV/VSQRT cut short by a short interrupt | Lazy FPU stacking off (`FPCCR.LSPEN = 0`) |
| 2.1.3 wrong vector after a store before an interrupt return | Every handler ends with `__DSB()` |
| 2.2.7 first SRAM write after a badly timed reset | `bsp/startup_stm32g474xx.s` reads each SRAM block before anything writes |
| 2.7.9 a conversion > 1 ms after the last (or after calibration) can be wrong; 2.7.10 (rev Y) channel 0 converted instead of the requested one after ADSTP or a finished DMA | One thrown-away conversion before every capture, and before VREFINT |
| 2.7.8 (rev Z) switching an ADC's input disturbs the others | Every ADC converts one channel; only VREFINT switches ADC1's input, at idle |
| 2.7.11 one ADC disturbing another | No two ADCs ever convert at the same time |

Not applicable here: Standby with HSE bypass (2.2.5/2.2.10, no low-power modes), VREFBUF (2.8.1, the Nucleo's
reference is used), COMP (2.9.1, the LT1719 is used), the timer errata (none concerns input capture).
**ES0430 rev 9 covers silicon Z, Y and X; MB1367-D01 boards may carry revision "2", which it does not cover.**
The banner prints the revision (`silicon=`).

## Hardware facts the code relies on (checked 2026-10-15 in the documents in `docs/`)

| Fact | Value | Source |
|---|---|---|
| Record input | PA0 = ADC12_IN1 (fast channel), TT_a | DS12288 Table 12 p59 |
| I_SENSE input | PA1 = ADC12_IN2: ADC2 reads it, so ADC1 stays set for the record | DS12288 p59 |
| XFER_SENSE input | PB0 = ADC3_IN12 / ADC1_IN15 (slow channels), TT_a; VIL 0.99 V, VIH 2.31 V at 3.3 V (0.7/0.3 VDD; by design 1.23 / 1.88 V); no positive injection | DS12288 p61, Table 54 p128, Table 15 p83 |
| Comparator input | PB10 = TIM2_CH3, AF1 | DS12288 Table 13 p74 |
| Emulator output | PA4 = DAC1_OUT1 (also Arduino A2) | DS12288 p60 |
| Console | LPUART1, PA2 TX / PA3 RX, AF12, SB17/SB23 fitted; STLINK-V3E; its baud limits are in UM2448 (not here) | UM2505 Tables 10-12 p24-26 |
| LD2, B1 | PA5 active high; PC13 high when pressed (100 kohm pull-down) | UM2505 §7.6.1-7.6.2 p23 |
| GPS UART | PC5 = USART1_RX, PC4 = USART1_TX, both AF7; on Arduino D0 / D1 by default (SB19 / SB13 on), also CN10-6 (SB3) and CN10-34 (SB33) | DS12288 Table 15 p75; UM2505 Table 10 p24, Tables 12 p25-26, 15 p30-31, 16 p32-33 |
| GPS PPS | PB3 = TIM2_CH2, AF1 (JTDO/TRACESWO at reset; SWD stays on PA13/PA14); Arduino D3 = CN9-4, also CN10-31; T_SWO on SB15 | DS12288 Table 14 p74; UM2505 Table 12 p26, Table 15 p31 |
| HSE | X3, 24 MHz crystal (SB25/SB26 fitted). TCXO: SB28 on, SB25/SB26 off, SB24/SB27 off, logic-level input | UM2505 §7.5.1 p22, Table 12 p26 |
| VREF+ | TL1431 via JP8 [1-2]: 3.26 V nominal, no tolerance given; measured at boot from VREFINT_CAL (0x1FFF75AA, at 3.0 V, 30 C) | UM2505 §7.6.3 p24; DS12288 Table 6 p33 |
| ADC clock | CKMODE = 11 (HCLK/4 = 42.5 MHz) is the only synchronous mode within 60 MHz (52 MHz with all ADCs running) | RM0440 §21.4.3; DS12288 Table 66 p138 |
| Sampling | 640.5 cycles allows 33 kohm on a slow channel (XFER_SENSE is 24.8 kohm); VREFINT needs >= 4 us | DS12288 Table 67 p141, Table 20 p87 |
| ADC trigger | TIM6_TRGO = EXTSEL 13 on ADC1/2 and ADC3/4/5 | RM0440 Tables 166-169 p617-621 |
| DMA requests | ADC1 5, ADC2 36, ADC3 37, TIM2_CH3 58; DMAMUX channel n drives DMA1 channel n+1 | RM0440 Table 93 p420 |
| 170 MHz | Range 1 boost, 4 wait states, AHB /2 for 1 us across the switch; PLL input 2.66-16 MHz, VCO 96-344 MHz | RM0440 Table 9 p98, §7.2.7 p279 |
| CCM SRAM | 32 KB at 0x10000000, DMA only through the 0x20018000 alias | RM0440 §2.4 p88 |
| IWDG | LSI 29.5-34 kHz; prescaler /32; DBG_IWDG_STOP in DBGMCU_APB1FZR1 bit 12 | DS12288 Table 45 p122; RM0440 §35.4.2, §47.16.4 |

## Build options

| CMake variable | Default | Use |
|---|---|---|
| `MAG_HSE_BYPASS` | OFF | ON for a TCXO on OSC_IN (the `tcxo` preset). HANDOFF items 25 and 27 |
| `MAG_HSE_HZ` | 24000000 | the TCXO's frequency, if not 24 MHz (a multiple of 4 MHz) |
| `MAG_VCP_BAUD` | 115200 | try 921600 for faster dumps once the terminal shows it works |
| `MAG_N5_SHDN_PB7` | OFF | rev C power board only: PB7 high at boot to enable -5 V_A |
| `MAG_GPS_TX` | OFF | ON drives USART1_TX on PC4 (D1) towards the GPS. Nothing sends on it yet |

## Tests on the PC

```bash
make -C tests/shot run
```
```bash
make -C tests/app run
```
```bash
make -C tests/fmt run
```
```bash
make -C tests/gps run
```
```bash
py -3.14 tests/tools/test_mag.py
```
`make` and `gcc` are Cygwin's (`C:\cygwin64\bin`).

| Test | What it covers |
|---|---|
| `tests/shot` | The sequencer on a simulated board: 19 scenarios, healthy and every fault, with the safety invariants checked on each |
| `tests/app` | The console end to end: commands in, report lines out, with the fit and Path A running on a synthetic decay and its comparator edges. Also checks the units, `run` stopping after three missed transfers, the dump framing and bad commands. With a simulated GPS (the MCU clock 25 ppm fast): FIT and PATHA give the true frequency back, `f_raw_Hz` the nominal one, and SHOT the UTC of sample 0 to 2 us and the position; and the `gps` commands |
| `tests/gps` | `app/gps.c` alone, from the interrupt entry points: the clock measurement, the UTC label (missed pulses, glitches, gaps over 20 s, late or early RMC, a 5 Hz receiver, midnight, new year, a ddmmyy date), positions in every quadrant, no fix, stale data, a clock change, bad checksums, the queues and their counters, raw echo; and the review's cases (a stale calibration after the clock moved, a late RMC, a glitch before its pulse's RMC, a 21 s main-loop stall). Sixteen deliberately broken versions of the GPS code, run against `tests/gps` and `tests/app`, were all caught |
| `tests/fit` | The fit and Path A against `frequency_estimation.py`: 27 cases x 20 records. The C fit equals `est_fit` to 2.5e-5 of the Cramer-Rao bound at worst, with the same rms error in every case. Run with `py -3.14 tests/fit/run_fit_tests.py`, about 45 s. Details in [tests/fit/README.md](tests/fit/README.md) |
| `tests/noise` | The per-shot uncertainty against the real scatter of f, over 80 records per case. With white noise, corrected sigma / rms error = 0.98 and the density ratio 1.01. With noise band-limited to 0.8-3 kHz the white sigma alone is 2.0-2.1x too small, and the corrected one gives 0.98 and 1.03 |
| `tests/fmt` | Formatting, parsing and the dump's CRC-32 |
| `tests/tools/test_mag.py` | Replays the firmware's output, a dump included, through `tools/mag.py` on a fake serial port, and checks the `--csv` survey row | The fit and Path A tests, which compare against
`frequency_estimation.py`, are described in [tests/fit/README.md](tests/fit/README.md).

## Decisions and deviations

- **The GPS (0.2.0, 2026-10-22).** Reviewed independently the same day: nothing in it can delay the gate paths, the
  sequencer or the record DMA. Its findings were fixed: an old clock measurement could lock out every later pulse
  after a gap; one glitch or late RMC could move the UTC; the sentence queue was shorter than a dump; `mag.py`'s CSV
  had no `mode` and could reuse an earlier shot's FIT line.

- **LL and registers, not HAL.** The plan said "HAL sets the peripherals up". Every peripheral here is configured at
  register level with CMSIS names, and the LL headers are kept for reference. The timed paths needed register
  control anyway, and the code stays small (57 KB) with nothing hidden.
- **Console at 115200.** The bridge's limits are undocumented here, so this is the conservative default; a 60 KB
  dump then takes about 5 s.
- **`no_fid` result.** A record needs a fitted amplitude of at least 0.5 x the noise rms (config.h
  `FID_MIN_SNR`), as the README asks, to catch a K_XFER contact that never made. The weakest computed case
  (untuned, 25 uT) is 1.5.
- **XFER thresholds.** These are the README's 2.31 V / 0.99 V, which the datasheet confirms as the 0.7/0.3 VDD
  characterization values.
- **No "idle ADC code" at boot.** The README's boot step lists it, but nothing depends on it: the fit estimates
  each record's offset itself.
- **The I_SENSE trip needs 4 conversions in a row above 1.6 V** (about 61 us), so a single noisy sample cannot
  abort a shot.

## Open items

1. Run [BRINGUP.md](BRINGUP.md) on a Nucleo: the clock, VREF+, the self-test, the line timing on a logic analyzer,
   the emulator faults, the overcurrent abort, and the record with a signal generator.
2. Measure the fit time on the M4F (the self-test prints it). Counted from the compiled code, not measured: about
   16 M cycles (0.09 s) for a typical fit (3-6 iterations) and 89 M (0.52 s) if all 40 iterations ran. The watchdog
   is refreshed every iteration, so even that is safe.
3. Bench the relays (rev D open item 3): K1's G6S times and bounce, then put the measured numbers into
   `config.h` and `xfer_model.c`.
4. Settle the console speed from UM2448, or by trying 921600.
5. The TCXO (HANDOFF item 25): build with `tcxo` once SB28/SB25/SB26 are changed.
6. **Path A's hysteresis bias.** The acquisition README says the firmware can remove it using the fitted envelope:
   shift each edge by asin(threshold / A(t)) / (2 pi f), with the measured thresholds (+5.5 / -13.8 mV at BP_OUT).
   Path A is a cross-check, so this is a refinement.
7. **A gate on-time limit independent of the CPU** (a hardware item, from the 2026-10-15 review). Today the limit
   comes from the TIM7 interrupt and the watchdog, and in a debug build a halted core stops both. Making it
   independent of the CPU needs hardware: for example, gate the driver's input with a monostable or an RC that
   cannot stay high longer than about 4.5 s. That would be a polarization-board change.
8. **The GPS on the bench** ([BRINGUP.md](BRINGUP.md) step 10): that the RMC follows its pulse (the UTC label
   depends on it), `clk_ppm` against step 8b's generator ratio, and BP_OUT's noise with the GPS on and off.
9. **The GPS's TX line is not used.** Configuring the receiver from the firmware (UBX) would need `MAG_GPS_TX` and
   code; today it is set up once in u-center and saved to its flash.
