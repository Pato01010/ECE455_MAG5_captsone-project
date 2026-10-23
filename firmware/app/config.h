/* config.h - every delay, threshold and constant of a shot, in one place.
 *
 * The specification is the "Firmware timing table" in
 * schematic/comparator-path/acquisition_rev_C/README.md (rev E keeps it without the boot step
 * "N5_SHDN HIGH": acquisition_rev_E/README.md), with the K1 times of
 * schematic/polarizaiton-switch/polarization_switch_rev_D/README.md.
 * Every delay is fixed from GATE_CMD falling (t = 0) or from RLY_CMD rising. Nothing is timed on I_SENSE.
 * A bench measurement changes one line here; say where the new number came from.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* ---- Time base -------------------------------------------------------------------------------- */
#define TICK_HZ             170000000u  /* TIM2 counts per second: SYSCLK 170 MHz, APB1 /1 */
#define TICKS_PER_US        170u
#define SAMPLE_HZ           20000u      /* ADC sample rate of the record and of XFER_SENSE */
#define TICKS_PER_SAMPLE    (TICK_HZ / SAMPLE_HZ)   /* 8500: TIM6 period */
#define US_PER_SAMPLE       (1000000u / SAMPLE_HZ)  /* 50 */

/* ---- Arming: before the gate (times from RLY_CMD rising unless stated) ------------------------- */
#define ARM_TO_RLY_US       2000    /* XFER_SENSE capture starts this long before RLY_CMD */
#define CHECK_A_FROM_US     (-1700) /* (a): XFER_SENSE high just before RLY_CMD, mean of this window */
#define CHECK_A_TO_US       (-100)
#define CHECK_A2_PRE_FROM_US (-1000) /* (a2): step = mean(post) - mean(pre) */
#define CHECK_A2_PRE_TO_US   0
#define CHECK_A2_POST_FROM_US 600
#define CHECK_A2_POST_TO_US   1000
#define CHECK_A2_AT_US      1100    /* evaluated once the +1.0 ms samples are in */
#define A2_MIN_STEP_MV      25      /* healthy 49-71 mV; 0 mV when the front end's Q2/R30 path is open */
#define CHECK_B_FROM_US     24000   /* (b): XFER_SENSE low = K1 operated (holds while NC opens <= 19.7 ms) */
#define CHECK_B_TO_US       25000
#define CHECK_B_AT_US       25100
#define RLY_TO_GATE_US      30000   /* GATE_CMD >= 30 ms after RLY_CMD (rev D README step 2) */
#define PRE_CAPTURE_SAMPLES 640     /* 32 ms at 20 kSPS: arm .. (b) */

/* ---- Polarization ------------------------------------------------------------------------------ */
#define T_POL_MIN_US        50000u      /* bench minimum; the specification is 2-4 s */
#define T_POL_MAX_US        4000000u    /* specification maximum */
#define GATE_GUARD_EXTRA_US 20000u      /* hardware guard ends the gate this long after t_pol */
#define ISENSE_ABORT_MV     1600    /* I_SENSE (1.000 V/A) above this: gate off at once */
#define ISENSE_LOW_MV       1000    /* below this: log it (expected 1.06-1.30 V), never abort on it */
#define ISENSE_LOG_END_US   10000   /* second I_SENSE reading this long before gate-off */

/* ---- After gate-off (t = 0 is GATE_CMD falling) ------------------------------------------------ */
#define RLY_OFF_US          500     /* RLY_CMD low; coil current is zero by +59-71 us */
#define TUNE_ON_US          5000    /* tuned shots only; TUNE is never high at gate-off */
#define POST_CAPTURE_SAMPLES 800    /* XFER_SENSE from +0.5 ms to +40.5 ms */
#define CHECK_CD_AT_US      40600
#define CHECK_C1_US         38000   /* (c): XFER_SENSE high at all three times */
#define CHECK_C2_US         39000
#define CHECK_C3_US         40000
#define CHECK_C_HALF_US     100     /* median of the samples within +/-0.1 ms of each time */
#define D_PLATEAU_FROM_US   1000    /* (d): median of samples above VIH, t_rise+1 .. t_rise+3 ms */
#define D_PLATEAU_TO_US     3000
#define D_HELD_FROM_US      38000   /* held = mean over +38 .. +40 ms */
#define D_HELD_TO_US        40000
#define D_MIN_STEP_MV       25      /* healthy 45-70 mV; below this K_XFER did not pick up */
#define D_PICKUP_MIN_US     2500    /* log-only flag outside 2.5-12 ms after t_rise */
#define D_PICKUP_MAX_US     12000
#define BLANK2_OFF_US       45000   /* BLANK2 low */
#define RECORD_START_US     60000   /* BP_OUT below 100 uV from +60.3 ms */
#define RECORD_SAMPLES      18800u  /* 0.94 s at 20 kSPS */
#define RECORD_END_US       (RECORD_START_US + RECORD_SAMPLES * US_PER_SAMPLE + 1000)
#define MAX_EDGES           6144u   /* both comparator edges at 65 uT: about 5,200 in 0.94 s */

/* ---- XFER_SENSE levels (PB0, divider 0.248 of XFER) -------------------------------------------- */
#define XFER_HIGH_MV        2310    /* "reads HIGH": VIH 0.7 VDD (healthy 2.75-3.06 V) */
#define XFER_LOW_MV         990     /* "reads LOW": VIL 0.3 VDD (healthy 0.06-0.41 V) */
#define MAX_MISSED_IN_A_ROW 3       /* after three missed transfers, stop and report K1's NC contact */

/* ---- Record and estimate ----------------------------------------------------------------------- */
#define FIT_F_LO_HZ         900.0   /* 25-65 uT is 1,064-2,768 Hz */
#define FIT_F_HI_HZ         3000.0
#define PATH_A_MIN_SNR      15.0    /* Path A only on tuned shots with peak/rms >= 15 at the record start */
/* A welded or dirty K_XFER contact passes every XFER check and leaves a record with no FID, so a
 * record also needs a fitted amplitude above the noise (README "The XFER readback"). Peak/rms 0.5
 * is still ~30 standard errors of the amplitude over 18,800 samples; the weakest computed case is 1.5. */
#define FID_MIN_SNR         0.5
#define RUN_GAP_US          1000000u /* default rest between shots of a run */
#define RUN_GAP_MAX_US      3600000000u /* longest rest a run accepts (one hour) */
/* Shielded-proton gyromagnetic ratio over 2 pi (H2O, sphere, 25 C; CODATA 2018), Hz per uT.
 * The free-proton value 42.5775 Hz/uT in the README is 26 ppm higher: about 1.3 nT at 50 uT. */
#define GAMMA_HZ_PER_UT     42.57638474

/* ---- GPS: u-blox NEO-F10N on USART1 (PC5) and TIM2_CH2 (PB3); app/gps.h, HANDOFF item 72 --------- */
#define GPS_BAUD            38400u      /* the NEO-F10N's default (SparkFun hookup guide) */
#define GPS_LAG_MAX_US      700000u     /* an RMC this soon after a PPS names that PPS's second; with only
                                           RMC + GGA on, a u-blox sends it ~0.1 s after (BRINGUP 10d) */
#define GPS_PPS_TOL_PPM     1000.0      /* a pulse this far from a whole number of nominal seconds is not part
                                           of the chain while the chain has no clock of its own: a crystal or
                                           TCXO passes, HSI16 (1 %) does not */
#define GPS_PPS_TOL_CAL_PPM 20.0        /* ... this far from the clock the chain has measured itself */
#define GPS_PPS_RESYNC      3u          /* so many such edges in a row start a new chain from the latest */
#define GPS_PPS_MAX_GAP_S   20u         /* missed pulses up to this gap are bridged by counting seconds */
#define GPS_CLK_WINDOW_S    16u         /* the clock is measured over the last 17 pulses */
#define GPS_CLK_MIN_S       4u          /* no correction from fewer seconds than this */
#define GPS_CLK_MAX_AGE_S   300u        /* a measurement older than this (pulses lost) is not applied */
#define GPS_UTC_MAX_EXTRAP_S 10.0       /* UTC only for times this close to the last pulse */
#define GPS_FIX_MAX_AGE_S   5u          /* a position older than this is not reported */

/* ---- ADC -------------------------------------------------------------------------------------- */
#define ADC_FULL_SCALE      4095u
#define VREF_NOMINAL_MV     3250u   /* the Nucleo's VREF+ (TL1431, JP8 [1-2]); measured at boot */

#endif
