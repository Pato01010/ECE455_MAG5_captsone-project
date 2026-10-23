# STM32G474RE documentation

**Collected:** 2026-10-14, for the control-subsystem firmware in `firmware/`

The microcontroller is the STM32G474RE on a NUCLEO-G474RE board (MB1367). The firmware specification is the
"Firmware timing table" in [`schematic/comparator-path/acquisition_rev_C/README.md`](../../schematic/comparator-path/acquisition_rev_C/README.md),
with the K1 relay times from [`schematic/polarizaiton-switch/polarization_switch_rev_D/README.md`](../../schematic/polarizaiton-switch/polarization_switch_rev_D/README.md).

Page numbers below are the PDF page numbers (ST prints the same number on each page).

## `mcu/` - the chip

| File | Document | Revision | Read it for |
|---|---|---|---|
| `DS12288_rev6_STM32G474xB-xC-xE_datasheet.pdf` | Datasheet, 236 pages | Rev 6, 16 Nov 2021 | LQFP64 pinout (§4.3, p50) and alternate functions (§4.11, p73); HSE bypass input limits for the TCXO (§5.3.7 External clock source characteristics, p115); ADC characteristics (§5.3.19, p138); comparator (§5.3.22, p163); timers (§5.3.26, p169); LQFP64 package (§6.4, p210) |
| `RM0440_rev9_STM32G4_reference_manual.pdf` | Reference manual, 2,140 pages | Rev 9, Mar 2025 | RCC (§7, p270) and the HSE clock (§7.2.1, p276); DMA (§12, p397) and DMAMUX (§13, p418); ADC (§21, p591), oversampler (§21.4.29, p651); VREFBUF (§23, p764); COMP (§24, p768); TIM2 (§30, p1287), input capture (§30.4.7, p1314); timers frozen while debugging (§47.16.2, p2108) |
| `ES0430_rev9_STM32G474_errata.pdf` | Errata for the STM32G471/473/474/483/484, 39 pages | Rev 9, Jun 2024 | Silicon bugs and workarounds - see "Errata to check" below |
| `PM0214_rev10_Cortex-M4_programming_manual.pdf` | Cortex-M4 programming manual, 262 pages | Rev 10, 23 Mar 2020 (see note) | The core: instruction set, FPU, NVIC interrupt priorities, SysTick |
| `STM32G474.svd` | Register description file | from STM32CubeCLT 1.21.0 | Peripheral register views in a debugger |

## `nucleo-board/` - the NUCLEO-G474RE

| File | Document | Revision | Read it for |
|---|---|---|---|
| `UM2505_rev7_NUCLEO-G474RE_user_manual.pdf` | User manual, STM32G4 Nucleo-64 boards (MB1367), 48 pages | Rev 7, May 2026 | Mechanical drawing (§7.2 Figure 6, p11 - HANDOFF open item 28); power supply (§7.4, p14); HSE clock options (§7.5.1, p22 - open item 27, the TCXO solder bridges); VREF+ selection (§7.6.3, p24 - open item 26); solder bridges (§7.7, p25); morpho connectors CN7 and CN10 (§8.4, p31); I/O assignment (§9, p34); product marking (§10.1, p36); known limitations (§10.2.1, p39); board revision history (§10.3, p39) |
| `MB1367-C04_schematic.pdf` | Board schematic, revision C-04, 8 pages | 25 Apr 2019 | Use the one schematic that matches the revision on your board (UM2505 §10.1 and §10.3 say how to read it) |
| `MB1367-C05_schematic.pdf` | Board schematic, revision C-05, 13 pages | 14 Feb 2022 | as above |
| `MB1367-D01_schematic.pdf` | Board schematic, revision D-01, 13 pages | 02 Jan 2026 | as above |

## `cube-software/` - the STM32CubeG4 package

Copies of files the STM32 tools installed with the STM32CubeG4 package 1.6.3 (`C:\Users\zongr\STM32Cube\Repository`),
so they match the libraries the firmware will build against. Nothing here was downloaded.

| File | Document | Read it for |
|---|---|---|
| `UM2492_rev3_STM32CubeG4_getting_started.pdf` | Getting started with STM32CubeG4, Rev 3, Sep 2024, 24 pages | How the package, examples and middleware are organized |
| `STM32G474xx_HAL_LL_drivers_CubeG4-1.6.3.chm` | HAL and LL driver reference for the STM32G474 | Every driver function and its parameters |
| `STM32G4xx_Nucleo_BSP_CubeG4-1.6.3.chm` | Nucleo board-support drivers | The board LED, button and virtual COM port |

## Errata to check for this design

From ES0430 Rev 9. None of the timer errata (§2.12) concern input capture.

- **§2.7.11 An ADC instance may impact the accuracy of another ADC instance at specific conditions (p17).** ADC1
  records the signal while ADC3 reads XFER_SENSE.
- **§2.7.8 ADC input channel switching disturbs ongoing conversions (p16).**
- **§2.7.9 Wrong ADC result if conversion done late after calibration or previous conversion (p17).**
- **§2.1.2 VDIV or VSQRT instructions might not complete correctly when very short ISRs are used (p5).** The
  frequency fit runs floating-point code while the edge-capture interrupt fires.
- **§2.2.5 and §2.2.10 Standby mode cannot be entered when HSE bypass is used (p8, p10).** The TCXO runs the HSE in
  bypass (open item 27). This matters only if the firmware uses Standby.
- **§2.9.1 Comparator previously not fully tested in production (p18).** Only if the internal COMP replaces the
  LT1719.
- **§2.8.1 VREF+ output voltage disturbed by some GPIOs toggling (p18).** Only if the internal VREFBUF replaces the
  Nucleo's reference (open item 26).

## Notes

- **PM0214:** ST's product page lists Rev 11 (28 Feb 2025), but the file ST serves at that link on 2026-10-14 is
  Rev 10 (23 Mar 2020), on its cover and in its revision table. Replace it if ST fixes the link.
- **Not downloaded** (the core set was chosen): the application notes on timers (AN4013, AN4776), ADC accuracy
  (AN2834, AN5346), clocks (AN6363), G4 hardware design (AN5093), comparators (AN4232) and VREFBUF (AN5690); RM0440
  in Markdown; the MB1367 manufacturing files (Gerbers). All are on the same two ST pages.
- **Before relying on a revision,** compare the cover of the file with the version on ST's product page.
- **How these were fetched:** st.com does not answer command-line requests from this laptop; the in-app browser
  does. HANDOFF trap 18 has the method.

## Sources

Product pages: [STM32G474RE](https://www.st.com/en/microcontrollers-microprocessors/stm32g474re.html) and
[NUCLEO-G474RE](https://www.st.com/en/evaluation-tools/nucleo-g474re.html). Files, renamed here by document number
and revision, with their content unchanged:

- [DS12288](https://www.st.com/resource/en/datasheet/stm32g474re.pdf)
- [RM0440](https://www.st.com/resource/en/reference_manual/rm0440-stm32g4-series-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
- [ES0430](https://www.st.com/resource/en/errata_sheet/es0430-stm32g471xx473xx474xx483xx484xx-device-errata-stmicroelectronics.pdf)
- [PM0214](https://www.st.com/resource/en/programming_manual/pm0214-stm32-cortexm4-mcus-and-mpus-programming-manual-stmicroelectronics.pdf)
- [UM2505](https://www.st.com/resource/en/user_manual/um2505-stm32g4-nucleo64-boards-mb1367-stmicroelectronics.pdf)
- [MB1367-C04](https://www.st.com/resource/en/schematic_pack/mb1367-g474re-c04_schematic.pdf),
  [MB1367-C05](https://www.st.com/resource/en/schematic_pack/mb1367-g474re-c05_schematic.pdf),
  [MB1367-D01](https://www.st.com/resource/en/schematic_pack/mb1367-g474re-d01-schematic.pdf)
