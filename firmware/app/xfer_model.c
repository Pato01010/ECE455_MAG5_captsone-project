/* xfer_model.c - the XFER_SENSE waveform of a shot.
 */
#include "xfer_model.h"

#include <math.h>
#include <string.h>

#include "config.h"

static bool after(uint32_t t, uint32_t ref) { return (int32_t)(t - ref) >= 0; }
static uint32_t ms_ticks(float ms) { return (uint32_t)(ms * 1000.0f * (float)TICKS_PER_US + 0.5f); }
static float ticks_ms(uint32_t dt) { return (float)dt / (1000.0f * (float)TICKS_PER_US); }

void xfer_model_rev_de(xfer_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->v_idle_mv = 2900.0f;
    m->release_step_mv = 61.0f;
    m->pick_step_mv = 57.0f;
    m->nc_open_ms = 5.0f;       /* G6S-2F slowed by R3/C8: NO makes ~7 ms after RLY_CMD */
    m->nc_make_ms = 3.4f;       /* NC feeds XFER at gate-off + 3.9 ms (RLY_CMD falls at +0.5 ms) */
    m->bounce_ms = 2.0f;        /* the G6S bounce is not measured yet */
    m->pick_ms = 7.7f;          /* transfer complete at gate-off + 11.6 ms */
}

float xfer_model_mv(const xfer_model_t *m, const xfer_history_t *h, uint32_t t)
{
    float v = m->v_idle_mv;

    if (m->xfer_dead) return 5.0f;
    if (!h->rly_rise_seen || !after(t, h->t_rly_rise)) return v;

    if (after(t, h->t_rly_rise + ms_ticks(0.12f))) v = m->v_idle_mv + m->release_step_mv;

    uint32_t t_open = h->t_rly_rise + ms_ticks(m->nc_open_ms);
    bool operated = !m->k1_stuck && (!h->rly_fall_seen || after(h->t_rly_fall, t_open));
    if (!operated) {
        /* K1 never operated: once RLY_CMD falls, K_XFER picks up again after its delay */
        if (h->rly_fall_seen && after(t, h->t_rly_fall + ms_ticks(m->pick_ms))) v = m->v_idle_mv;
        return v;
    }
    if (!after(t, t_open)) return v;

    v = 200.0f + (v - 200.0f) * expf(-ticks_ms(t - t_open) / 1.5f);    /* NC open: XFER decays */
    if (!h->rly_fall_seen || m->missed_transfer) return v;

    uint32_t t_make = h->t_rly_fall + ms_ticks(m->nc_make_ms);
    if (!after(t, t_make)) return v;
    float since = ticks_ms(t - t_make);
    v = m->v_idle_mv + m->release_step_mv;                              /* plateau: K_XFER not yet held */
    if (since < m->bounce_ms && fmodf(since, 0.9f) < 0.2f) v = 100.0f;  /* contact bounce */
    if (since >= m->pick_ms) v -= m->pick_step_mv;                      /* K_XFER picks up */
    if (m->drop_at_ms > 0.0f && h->gate_fall_seen && after(t, h->t_gate_fall + ms_ticks(m->drop_at_ms)))
        v = 0.0f;
    return v;
}
