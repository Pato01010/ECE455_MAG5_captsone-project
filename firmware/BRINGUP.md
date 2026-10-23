# Bring-up on the bench

**For:** firmware 0.2.0 (2026-10-22; 0.1.0 was 2026-10-15). Fill in the "Result" column as each step is done, with
the date.

Everything up to step 8 runs on a **bare NUCLEO-G474RE: no carrier, no stack**. Step 9 is the first time the
firmware meets the boards. Step 10 is the GPS: on the bare Nucleo any time after step 4, and once more on the stack.

Bench kit:
- the Nucleo and its USB cable;
- a logic analyzer or a four-channel scope;
- a signal generator with a TTL/sync output;
- a 10 kohm pot;
- jumper wires.

Commands are typed in `py -3.14 tools/mag.py term` (115200 baud), or sent one at a time with
`py -3.14 tools/mag.py cmd "<command>"`.

Pins used on the bare Nucleo, all on the morpho connector CN7:

| Signal | Pin | Morpho |
|---|---|---|
| TUNE | PC10 | CN7-1 |
| BLANK2 | PC11 | CN7-2 |
| GATE_CMD | PC12 | CN7-3 |
| RLY_CMD | PD2 | CN7-4 |
| ADC_PIN | PA0 | CN7-28 |
| I_SENSE | PA1 | CN7-30 |
| emulator out | PA4 | CN7-32 |
| XFER_SENSE | PB0 | CN7-34 |
| CMP_OUT | PB10 | CN10-25 |

GND is on CN7-8, -19, -20 and others.

**On a bare Nucleo, tie PA1 (I_SENSE, CN7-30) to GND through 10 kohm, or to the pot of step 8a.** Left floating, it
can read above 1.6 V and abort a shot as `overcurrent` at random. Dry runs are immune: they never arm the trip.

**Debugging:** a release build lets the watchdog run while a debugger halts the core, so a breakpoint resets the
board after 0.5 s. A debug build (`build.ps1 debug`) freezes it and says so in its banner. **Never halt or step a
debug build with the 12 V supply connected.**

| # | Step | Expected | Result |
|---|---|---|---|
| 1 | `build.ps1`, then `flash.ps1` | Builds clean; STM32_Programmer_CLI verifies and resets | |
| 2 | Open the terminal; press the black reset button | `MAGNETOMETER fw=0.2.0 clock=HSE_crystal vref_mV=... reset=reset-pin silicon=... watchdog=runs_when_halted`. `clock=HSI_1pct` means the HSE did not start (check X3, SB25, SB26). Note vref_mV: about 3260 (3250-3270 expected, HANDOFF item 26) | |
| 3 | Note the `silicon` field of the banner | Z, Y or X. `not_in_ES0430_rev9` means no column of the errata sheet covers this chip: get a newer ES0430 | |
| 4 | `selftest` | Three `SELFTEST` lines: snr=0 gives err_mHz about 0.000; snr=31.6 and 6.7 give an error of a few fsig_mHz at most. `ms` is the fit time on the M4F: write it down (estimate 160 ms) | |
| 5 | Logic analyzer on CN7-1/-2/-3/-4; power-cycle the board, then reset it, then flash it again | GATE_CMD, RLY_CMD and TUNE stay low through all three, with no glitch. BLANK2 goes high about 1 ms after reset (idle) | |
| 6 | `shot 0.05` with nothing on PB0 | `result=aborted reason=xfer_dead_a`, and RLY_CMD never rises. The board refuses to shoot without XFER | |
| 7a | `dry 3 tuned` | Nothing moves on the analyzer. The `TIME` line gives the planned times as measured: rly_off_us 500, tune_on_us 5000, blank2_off_us 45000, rec0_us about 60052, rec_end_us about 1001000, tpol_us 3000000 | |
| 7b | Jumper PA4 (CN7-32) to PB0 (CN7-34); `emu none`; `shot 3 tuned` | On the analyzer:<br>- RLY_CMD rises 2 ms after the command; GATE_CMD 30 ms later, for 3.000 s<br>- RLY_CMD falls 0.5 ms after GATE_CMD; TUNE rises at +5 ms<br>- BLANK2 falls at +45 ms; TUNE and BLANK2 return at +1.001 s<br>Reports:<br>- `CHECK`: a_mV about 2900, a2_mV about +61, b_mV about 200-300, c1..c3 about 2900, d_step_mV about 57, d_pick_ms about 7.7<br>- `SHOT`: result=no_fid with nothing on PA0 (correct: no decay was recorded) | |
| 7c | `emu dead`, `emu no_release`, `emu k1_stuck`, `emu missed`, `emu no_pickup`, `emu drop` (one `shot 0.2` each) and `emu rev_c` | Reasons, in order:<br>- dead: xfer_dead_a<br>- no_release: no_release_step_a2, with RLY_CMD falling 1.1 ms after it rose<br>- k1_stuck: k1_not_operated_b at 25.1 ms; GATE_CMD never rises<br>- missed: missed_transfer_c<br>- no_pickup: no_pickup_d<br>- drop: xfer_dropped<br>- rev_c: like `none`, with d_pick_ms about 9.5 | |
| 7d | `emu none`; `run 5 0.2` with PB0 unplugged after shot 2 | The run continues, then stops: "RUN stopped: shot aborted" | |
| 8a | Pot between 3V3 and GND, wiper into PA1 (CN7-30) set near 1.2 V; `emu none`; `shot 3`; turn the pot past 1.6 V during the 3 s | GATE_CMD falls within about 0.1 ms of the crossing; RLY_CMD falls 0.5 ms later; `result=aborted reason=overcurrent`. With the pot below 1.0 V the shot completes and `i_end_mV` shows the reading | |
| 8b | Signal generator, sine, 2.128875 kHz, 1 Vpp around 1.6 V, into PA0 (CN7-28); its TTL sync into PB10 (CN10-25); `emu none`; `shot 0.2 tuned dump` | `FIT` f_Hz equals the generator frequency to within the clock's error, with status=-4 and T2_s 1000: a steady sine pins the decay at its floor, which is right for a generator, and so `SHOT` says no_fid. `PATHA` still runs and agrees with `FIT` to a few mHz. `mag.py` saves the dump and its Python refit agrees with the firmware's f to a small fraction of fsig. **The ratio firmware / generator is the Nucleo crystal's error in ppm: compare it with a counter-checked or GPS-locked generator** (HANDOFF item 2). Without the GPS connected, `FIT` says `corr=none` and `f_raw_Hz` equals `f_Hz` | |
| 8c | B1 during a 3 s shot | GATE_CMD falls at once; RLY_CMD 0.5 ms later; `reason=user_abort` | |
| 9 | Only on rev E boards with XFER on bus pin 13 and the J4 continuity checks done (acquisition rev E README, open item 2). **JP5 to E5V, J3 fitted.** First `dry 3 tuned`, then one `shot 2` | `dry`: (a) reads HIGH on the real XFER. Then the bench list: K1 operate/release/bounce, the (a2) step, K_XFER pickup, the BLANK2 transient, the first FID untuned | |

### Step 10: the GPS (HANDOFF item 72)

Kit: the SparkFun NEO-F10N breakout, an active SMA GNSS antenna with a view of the sky (a window works, slowly), a
laptop with u-center. Wiring, as in README "GPS": 5V to CN6-5, GND to CN6-6, the GPS's TX to D0 (CN9-1), PPS to D3
(CN9-4). D1 stays unconnected.

| # | Step | Expected | Result |
|---|---|---|---|
| 10a | Before wiring: the breakout alone on USB-C in u-center. Wait for a fix. Leave only RMC and GGA on UART1 at 1 Hz (recommended); optionally enable L5 and its health override. Save to flash. Then cut the USB-TX and USB-RX jumpers | A 3D fix; the PPS LED blinks once a second; 38400 baud on UART1 | |
| 10b | Wire it to the Nucleo; `gps` twice, a few seconds apart | `rmc` and `gga` go up by one a second; `bad_sum` 0, `dropped` 0, `too_long` 0. Within a minute: `fix=1`, `sats` 4 or more, `lat`/`lon` where you are | |
| 10c | `gps` after 20 s with a fix | `pps` counting, `pps_bad` 0, `clk_s=16`, `clk_ppm` = the Nucleo crystal's error (write it down), `lag_ms` about 100 with only RMC and GGA on (it must stay below 700, or no UTC), `labels` counting, `utc_jumps` 0. `utc` within a second of a phone's clock | |
| 10d | Logic analyzer on D3 (PPS) and D0 (the GPS's TX), decoding the UART at 38400 | **Each RMC starts after the pulse of the second it names**, and the pulse-to-`$GNRMC` delay equals `lag_ms`. If an RMC came before its pulse, `utc` would be one second late: stop and report it | |
| 10e | The generator of step 8b on PA0 / PB10; `shot 0.2 tuned` | `SHOT`: `clk_ppm` as in 10c, `utc` and the position filled in. `FIT`: `corr=gps`; `f_Hz` equals the generator to its own accuracy, and `f_raw_Hz / f_Hz - 1` = -`clk_ppm` x 1e-6. `clk_ppm` agrees with step 8b's ratio within the generator's accuracy | |
| 10f | Cover or unplug the antenna for 10 s, then restore it | After 5 s `gps` shows `fix=0` and `lat=na`; `clk_ppm` stays (kept 300 s). After it is restored: the chain restarts (`pps_gaps` or `pps_bad` may count one), `labels` resume, `utc` correct again | |
| 10g | On the stack (step 9 done): a few `dry 0.2` / record shots with the GPS powered, then unpowered | `noise_mV` in `FIT` the same within its scatter: the GPS's UART bursts and its supply current do not reach BP_OUT | |

When a measured relay time or level differs from `app/config.h` or `app/xfer_model.c`, change it there,
note the source, and re-run `make -C tests/shot run`.
