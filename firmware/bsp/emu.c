/* emu.c - bench emulator: DAC1 channel 1 (PA4) plays XFER_SENSE as the boards would.
 *
 * BENCH ONLY, on a bare Nucleo: jumper PA4 (CN7-32) to PB0 (CN7-34). Never with the carrier fitted.
 * The main loop watches RLY_CMD and GATE_CMD and writes the waveform of app/xfer_model.c (the one
 * the PC tests use) to the DAC, so a whole shot - every XFER check, the ADC3 captures, the EXTI
 * watch - runs on the bench the way it ran on the PC. A fault name makes the next shots fail the
 * way that fault would.
 */
#include "emu.h"

#include <string.h>

#include "board.h"
#include "hw.h"
#include "xfer_model.h"

static bool on;
static xfer_model_t model;
static xfer_history_t hist;
static bool rly_prev, gate_prev;
static float codes_per_mv;

int emu_start(uint32_t vref_mv, const char *fault)
{
    xfer_model_rev_de(&model);
    if (fault == NULL || strcmp(fault, "none") == 0) {}
    else if (strcmp(fault, "dead") == 0) model.xfer_dead = true;
    else if (strcmp(fault, "no_release") == 0) model.release_step_mv = 0.0f;
    else if (strcmp(fault, "k1_stuck") == 0) model.k1_stuck = true;
    else if (strcmp(fault, "missed") == 0) model.missed_transfer = true;
    else if (strcmp(fault, "no_pickup") == 0) model.pick_step_mv = 0.0f;
    else if (strcmp(fault, "drop") == 0) model.drop_at_ms = 500.0f;
    else if (strcmp(fault, "rev_c") == 0) {
        model.nc_open_ms = 10.0f;   /* the slow G5LE of rev C */
        model.nc_make_ms = 15.0f;
        model.bounce_ms = 7.2f;
        model.pick_ms = 9.5f;
    } else return -1;

    memset(&hist, 0, sizeof hist);
    rly_prev = hw_get(OUT_RLY);
    gate_prev = hw_get(OUT_GATE);
    codes_per_mv = 4095.0f / (float)vref_mv;

    RCC->AHB2ENR |= RCC_AHB2ENR_DAC1EN;
    (void)RCC->AHB2ENR;
    pin_mode(GPIOA, 4u, 3u);
    DAC1->CR = 0;
    DAC1->MCR = (2u << DAC_MCR_HFSEL_Pos);  /* HCLK above 160 MHz; MODE1 = 000: buffered, to the pin */
    DAC1->CR = DAC_CR_EN1;
    board_delay_us(10);
    on = true;
    emu_poll();
    return 0;
}

void emu_stop(void)
{
    on = false;
    DAC1->CR = 0;                           /* PA4 back to a plain analog pin */
}

bool emu_on(void)
{
    return on;
}

void emu_poll(void)
{
    if (!on) return;
    const uint32_t now = hw_now();
    const bool rly = hw_get(OUT_RLY), gate = hw_get(OUT_GATE);
    if (rly && !rly_prev) {                 /* a new shot: forget the last one */
        memset(&hist, 0, sizeof hist);
        hist.rly_rise_seen = true;
        hist.t_rly_rise = now;
    }
    if (!rly && rly_prev) { hist.rly_fall_seen = true; hist.t_rly_fall = now; }
    if (!gate && gate_prev) { hist.gate_fall_seen = true; hist.t_gate_fall = now; }
    rly_prev = rly;
    gate_prev = gate;

    float code = xfer_model_mv(&model, &hist, now) * codes_per_mv + 0.5f;
    DAC1->DHR12R1 = code < 0.0f ? 0u : (code > 4095.0f ? 4095u : (uint32_t)code);
}
