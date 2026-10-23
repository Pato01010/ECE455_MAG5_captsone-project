/* shot.h - one polarize / switch-off / record cycle, run from a timer compare interrupt.
 */
#ifndef SHOT_H
#define SHOT_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "xfer.h"

typedef enum {
    R_NONE = 0,
    R_USER_ABORT,
    R_XFER_DEAD,          /* (a): XFER low before RLY_CMD - no shot */
    R_NO_RELEASE_STEP,    /* (a2): no step 1 ms after RLY_CMD - K_XFER did not release */
    R_K1_NOT_OPERATED,    /* (b): XFER still high 25 ms after RLY_CMD */
    R_OVERCURRENT,        /* I_SENSE above ISENSE_ABORT_MV */
    R_GATE_GUARD,         /* the hardware guard ended the gate */
    R_CLOCK,              /* the HSE clock failed during the shot */
    R_MISSED_TRANSFER,    /* (c): record invalid */
    R_NO_PICKUP,          /* (d): record invalid */
    R_XFER_DROPPED,       /* XFER fell during the record: record invalid */
    R_RECORD_SHORT,       /* the record DMA did not finish */
    R_COUNT
} shot_reason_t;

/* Bits for shot_fault(), called from interrupts that have already made the line safe. */
#define FAULT_OVERCURRENT   0x1u
#define FAULT_GUARD         0x2u
#define FAULT_XFER_DROP     0x4u
#define FAULT_CLOCK         0x8u

typedef enum {
    EV_ARM, EV_RLY_ON, EV_GATE_ON, EV_GATE_OFF, EV_RLY_OFF, EV_TUNE_ON, EV_BLANK2_OFF,
    EV_REC_START, EV_REC_END, EV_COUNT
} shot_event_t;

typedef struct {
    uint32_t t_pol_us;    /* gate-on time, T_POL_MIN_US .. T_POL_MAX_US */
    bool tuned;           /* TUNE high from +5 ms to the end of the record */
    bool dry;             /* control lines left idle; checks measured and logged, not enforced */
} shot_params_t;

typedef struct {
    shot_params_t p;
    bool aborted;         /* stopped before the record */
    bool valid;           /* the record can be used */
    shot_reason_t reason; /* first problem found, R_NONE if none */

    float a_mv, a2_step_mv, b_mv;
    bool a_ok, a2_ok, b_ok;
    bool a_ran, a2_ran, b_ran, cd_ran;  /* which checks were evaluated at all */
    uint16_t i_mid_code, i_end_code;
    bool i_mid_done, i_end_done;        /* the I_SENSE readings were taken */
    float c_mv[3];
    bool c_ok;
    xfer_d_t d;
    bool watch_tripped;

    bool ev_done[EV_COUNT];
    uint32_t ev_t[EV_COUNT];    /* TIM2 time of each event */
    uint32_t t_rly, t_gate_on, t_gate_off;
    uint32_t t_pre0, t_post0, t_rec0;   /* time of sample 0 of each capture */
    uint32_t n_pre, n_post, n_rec, n_edges;
    bool edges_overflow;
} shot_result_t;

/* Buffers belong to the caller; mv_per_code = VREF+ / 4095 in mV. */
void shot_init(uint16_t *pre, uint16_t *post, uint16_t *rec, uint32_t *edges, float mv_per_code);
void shot_set_mv_per_code(float mv_per_code);
bool shot_start(const shot_params_t *p);    /* false if busy or the parameters are out of range */
void shot_abort(void);
bool shot_busy(void);                       /* true until the result has been taken */
bool shot_take_result(shot_result_t *out);  /* true once per finished shot */

void shot_tick(void);                       /* timer compare interrupt */
void shot_fault(uint32_t bits);             /* any interrupt priority */

/* Signed microseconds from b to a. */
static inline int32_t shot_us(uint32_t a, uint32_t b) { return (int32_t)(a - b) / (int32_t)TICKS_PER_US; }

const char *shot_reason_name(shot_reason_t r);

#endif
