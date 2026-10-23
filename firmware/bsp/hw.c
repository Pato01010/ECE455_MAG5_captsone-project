/* hw.c - captures, timers and interrupts behind app/hw.h, on the STM32G474RE.
 *
 * Time base: TIM2, 32-bit, free-running at 170 MHz. Compare channel 1 wakes the sequencer;
 * channel 2 captures the GPS's PPS rising edge (PB3) at all times, into gps_pps_isr();
 * channel 3 captures both comparator edges (PB10) by DMA during the record.
 * Sample clock: TIM6 update (TRGO) at 20 kHz triggers ADC1 (the record) or ADC3 (XFER_SENSE);
 * the two captures never overlap, so one timer serves both.
 * I_SENSE: ADC2 converts PA1 continuously while the gate is on; its analog watchdog drops
 * GATE_CMD from the interrupt at once. Gate guard: TIM7 one-pulse, same priority.
 * Errata (ES0430 rev 9) handled here:
 *  2.7.11 one ADC disturbing another: no two ADCs ever convert at the same time (ADC2 runs only while
 *         the gate is on, ADC3 only around RLY_CMD and the transfer, ADC1 only in the record);
 *  2.7.9  a conversion more than 1 ms after the previous one (or the calibration) can be wrong, and
 *  2.7.10 (rev Y) channel 0 can be converted instead of the requested one after ADSTP or after the DMA
 *         finishes: every capture starts with one dummy conversion that is thrown away;
 *  2.7.8  (rev Z) switching an ADC's input multiplexer disturbs conversions on the others: every ADC
 *         converts a single channel; only VREFINT switches ADC1's input, at idle;
 *  2.1.3  wrong vector after a store just before an interrupt return: every handler ends with a DSB.
 */
#include "hw.h"

#include "board.h"
#include "config.h"
#include "gps.h"
#include "shot.h"

/* DMAMUX1 channel n feeds DMA1 channel n+1. */
#define DMA_REC     DMA1_Channel1
#define MUX_REC     DMAMUX1_Channel0
#define DMA_EDGES   DMA1_Channel2
#define MUX_EDGES   DMAMUX1_Channel1
#define DMA_XFER    DMA1_Channel3
#define MUX_XFER    DMAMUX1_Channel2
#define DMA_ISNS    DMA1_Channel4
#define MUX_ISNS    DMAMUX1_Channel3

/* DMAMUX request inputs (stm32g4xx_ll_dmamux.h) */
#define REQ_ADC1        5u
#define REQ_ADC2        36u
#define REQ_ADC3        37u
#define REQ_TIM2_CH3    58u

#define EXTSEL_TIM6_TRGO 13u    /* ADC_CFGR EXTSEL code for TIM6_TRGO, ADC1/2 and ADC3/4/5 */

/* ADC sampling-time codes */
#define SMP_92_5    5u
#define SMP_640_5   7u

/* The ADCs run from HCLK/4 = 42.5 MHz, so one ADC clock is 4 TIM2 counts. A sample is taken at
 * the end of its sampling window: that delay is added to the time of sample 0. */
#define REC_SMP         SMP_92_5
#define REC_DELAY       370u        /* 92.5 x 4 */
#define XFER_SMP        SMP_640_5   /* 24.8 kohm source (R11 || R12) */
#define XFER_DELAY      2562u       /* 640.5 x 4 */
#define ISNS_SMP        SMP_640_5
#define ISNS_N          64u         /* about 1 ms of I_SENSE at 65 kSPS */
#define ISNS_AWD_FILTER 3u          /* AWD1 needs 4 consecutive conversions above the threshold */
#define GUARD_CAP_10KHZ 45000u      /* the guard never allows more than 4.5 s, whatever it is asked */

static uint16_t isense_buf[ISNS_N];
static uint32_t xfer_n, rec_n, edges_n;

/* ---- Small helpers ------------------------------------------------------------------------------ */
/* ADC_CR bits are set-only: write the one bit wanted, never read-modify-write. */
static void adc_cr(ADC_TypeDef *a, uint32_t bit)
{
    a->CR = ADC_CR_ADVREGEN | bit;
}

/* Bounded waits: a peripheral that never answers must not hang an interrupt (the watchdog would). */
#define WAIT_LOOPS 100000u      /* about 1 ms at 170 MHz; an ADC answers in microseconds */

static void adc_stop(ADC_TypeDef *a)
{
    if (a->CR & ADC_CR_ADSTART) {
        adc_cr(a, ADC_CR_ADSTP);
        for (uint32_t i = 0; (a->CR & ADC_CR_ADSTP) && i < WAIT_LOOPS; i++) {}
    }
}

static void adc_smp(ADC_TypeDef *a, uint32_t ch, uint32_t smp)
{
    if (ch < 10u) a->SMPR1 = (a->SMPR1 & ~(7u << (3u * ch))) | (smp << (3u * ch));
    else a->SMPR2 = (a->SMPR2 & ~(7u << (3u * (ch - 10u)))) | (smp << (3u * (ch - 10u)));
}

static void adc_power_up(ADC_TypeDef *a)
{
    a->CR = 0;                          /* leave deep power-down */
    a->CR = ADC_CR_ADVREGEN;
    board_delay_us(20);                 /* regulator start-up */
    adc_cr(a, ADC_CR_ADCAL);            /* single-ended calibration */
    while (a->CR & ADC_CR_ADCAL) {}
    board_delay_us(1);
    a->ISR = ADC_ISR_ADRDY;
    adc_cr(a, ADC_CR_ADEN);
    while (!(a->ISR & ADC_ISR_ADRDY)) {}
    a->ISR = ADC_ISR_ADRDY;
}

/* One software-triggered conversion, thrown away, just before a capture (ES0430 2.7.9 / 2.7.10). */
static void adc_dummy(ADC_TypeDef *a)
{
    const uint32_t cfgr = a->CFGR;
    a->CFGR = cfgr & ~(ADC_CFGR_EXTEN | ADC_CFGR_EXTSEL | ADC_CFGR_DMAEN | ADC_CFGR_CONT);
    a->ISR = ADC_ISR_EOC;
    adc_cr(a, ADC_CR_ADSTART);
    for (uint32_t i = 0; !(a->ISR & ADC_ISR_EOC) && i < WAIT_LOOPS; i++) {}
    (void)a->DR;
    adc_stop(a);                        /* CFGR can be written only with ADSTART = 0 */
    a->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
    a->CFGR = cfgr;
}

/* One channel, triggered by TIM6, one-shot DMA, 12-bit right-aligned. */
static void adc_triggered(ADC_TypeDef *a, uint32_t ch, uint32_t smp)
{
    a->CFGR = ADC_CFGR_DMAEN | (EXTSEL_TIM6_TRGO << ADC_CFGR_EXTSEL_Pos) | ADC_CFGR_EXTEN_0 | ADC_CFGR_OVRMOD;
    a->SQR1 = ch << ADC_SQR1_SQ1_Pos;
    adc_smp(a, ch, smp);
}

static void dma_start(DMA_Channel_TypeDef *ch, DMAMUX_Channel_TypeDef *mux, uint32_t req,
                      volatile void *periph, void *mem, uint32_t n, bool word, bool circular)
{
    ch->CCR = 0;
    mux->CCR = req << DMAMUX_CxCR_DMAREQ_ID_Pos;
    ch->CPAR = (uint32_t)periph;
    ch->CMAR = (uint32_t)mem;
    ch->CNDTR = n;
    ch->CCR = (word ? (DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1) : (DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0))
              | DMA_CCR_MINC | DMA_CCR_PL | (circular ? DMA_CCR_CIRC : 0u) | DMA_CCR_EN;
}

/* Start the 20 kHz sample clock; returns the TIM2 time of its first trigger. */
static uint32_t sample_clock_start(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    TIM6->CNT = 0;
    uint32_t t = TIM2->CNT;
    TIM6->CR1 = TIM_CR1_CEN;
    __set_PRIMASK(primask);
    return t + TICKS_PER_SAMPLE;
}

static void sample_clock_stop(void)
{
    TIM6->CR1 = 0;
}

/* ---- Init --------------------------------------------------------------------------------------- */
void hw_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
    RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN | RCC_AHB2ENR_ADC345EN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN | RCC_APB1ENR1_TIM6EN | RCC_APB1ENR1_TIM7EN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    RCC->CCIPR = (RCC->CCIPR & ~(RCC_CCIPR_ADC12SEL | RCC_CCIPR_ADC345SEL))
                 | (2u << RCC_CCIPR_ADC12SEL_Pos) | (2u << RCC_CCIPR_ADC345SEL_Pos);   /* SYSCLK */
    (void)RCC->CCIPR;

    /* ADCs: synchronous HCLK/4 = 42.5 MHz (a fixed trigger-to-sample delay), VREFINT on ADC1 */
    ADC12_COMMON->CCR = (3u << ADC_CCR_CKMODE_Pos) | ADC_CCR_VREFEN;
    ADC345_COMMON->CCR = (3u << ADC_CCR_CKMODE_Pos);
    adc_power_up(REC_ADC);
    adc_power_up(ISNS_ADC);
    adc_power_up(XFER_ADC);
    adc_triggered(REC_ADC, REC_CHANNEL, REC_SMP);
    adc_triggered(XFER_ADC, XFER_CHANNEL, XFER_SMP);
    adc_smp(ISNS_ADC, ISNS_CHANNEL, ISNS_SMP);

    /* TIM6: 20 kHz update -> TRGO */
    TIM6->CR1 = 0;
    TIM6->PSC = 0;
    TIM6->ARR = TICKS_PER_SAMPLE - 1u;
    TIM6->CR2 = TIM_CR2_MMS_1;
    TIM6->CR1 = TIM_CR1_URS;
    TIM6->EGR = TIM_EGR_UG;             /* load PSC: no ADC is armed yet, so the trigger is harmless */
    TIM6->SR = 0;

    /* TIM7: the gate guard, 10 kHz one-pulse */
    TIM7->CR1 = 0;
    TIM7->PSC = (TICK_HZ / 10000u) - 1u;
    TIM7->ARR = 1;
    TIM7->CR1 = TIM_CR1_OPM | TIM_CR1_URS;
    TIM7->EGR = TIM_EGR_UG;
    TIM7->SR = 0;
    TIM7->DIER = TIM_DIER_UIE;

    /* TIM2: 32-bit time base; CC1 = compare for the sequencer; CC2 = the PPS, rising edge, always on;
     * CC3 = input capture, both edges (enabled during the record). The PPS filter (fDTS/32, N = 8: an edge
     * must hold 1.5 us) rejects short spikes on the jumper wire, for a fixed delay that cancels in the rate. */
    TIM2->CR1 = 0;
    TIM2->PSC = 0;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->CCMR1 = TIM_CCMR1_CC2S_0 | (15u << TIM_CCMR1_IC2F_Pos);  /* CC1 compare; TI2, filter 1.5 us */
    TIM2->CCMR2 = TIM_CCMR2_CC3S_0 | (3u << TIM_CCMR2_IC3F_Pos);   /* TI3, filter 8 x fCK (47 ns) */
    TIM2->CCER = TIM_CCER_CC3P | TIM_CCER_CC3NP | TIM_CCER_CC2E;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0;
    TIM2->DIER = TIM_DIER_CC1IE | TIM_DIER_CC2IE;
    TIM2->CR1 = TIM_CR1_CEN;

    /* ES0430 2.1.2: no lazy FPU stacking, so a short handler cannot cut a VDIV/VSQRT short */
    FPU->FPCCR &= ~FPU_FPCCR_LSPEN_Msk;

    NVIC_SetPriority(ADC1_2_IRQn, PRIO_SAFETY);
    NVIC_SetPriority(TIM7_DAC_IRQn, PRIO_SAFETY);
    NVIC_SetPriority(TIM2_IRQn, PRIO_SEQUENCER);
    NVIC_SetPriority(EXTI0_IRQn, PRIO_SEQUENCER);
    NVIC_EnableIRQ(ADC1_2_IRQn);
    NVIC_EnableIRQ(TIM7_DAC_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn);
    NVIC_EnableIRQ(EXTI0_IRQn);
}

/* VREF+ = 3.0 V x VREFINT_CAL / VREFINT, averaged over 64 conversions. Only while no shot runs. */
uint32_t hw_vref_mv(void)
{
    const uint16_t cal = *(const uint16_t *)0x1FFF75AAu;   /* VREFINT_CAL, measured at VREF+ = 3.0 V */
    uint32_t sum = 0;

    adc_stop(REC_ADC);
    REC_ADC->CFGR = ADC_CFGR_OVRMOD;    /* software trigger, no DMA */
    REC_ADC->SQR1 = 18u << ADC_SQR1_SQ1_Pos;
    adc_smp(REC_ADC, 18u, SMP_640_5);       /* 15 us: VREFINT needs at least 4 us */
    for (int i = 0; i <= 64; i++) {
        REC_ADC->ISR = ADC_ISR_EOC;
        adc_cr(REC_ADC, ADC_CR_ADSTART);
        while (!(REC_ADC->ISR & ADC_ISR_EOC)) {}
        uint32_t v = REC_ADC->DR;
        if (i > 0) sum += v;                /* the first conversion may be wrong (ES0430 2.7.9) */
    }
    adc_triggered(REC_ADC, REC_CHANNEL, REC_SMP);
    if (sum == 0u) return VREF_NOMINAL_MV;
    return (uint32_t)((3000ull * cal * 64u + sum / 2u) / sum);
}

/* ---- Control lines ------------------------------------------------------------------------------ */
static GPIO_TypeDef *const out_port[OUT_COUNT] = { GATE_PORT, RLY_PORT, TUNE_PORT, BLANK2_PORT };
static const uint32_t out_pin[OUT_COUNT] = { GATE_PIN, RLY_PIN, TUNE_PIN, BLANK2_PIN };

void hw_set(hw_out_t o, bool high)
{
    if (high) pin_high(out_port[o], out_pin[o]);
    else pin_low(out_port[o], out_pin[o]);
}

bool hw_get(hw_out_t o)
{
    return pin_out(out_port[o], out_pin[o]);
}

/* ---- Time --------------------------------------------------------------------------------------- */
uint32_t hw_now(void)
{
    return TIM2->CNT;
}

void hw_wake_at(uint32_t t)
{
    TIM2->CCR1 = t;
    if ((int32_t)(t - TIM2->CNT) <= 0) TIM2->EGR = TIM_EGR_CC1G;   /* already due: interrupt now */
}

void TIM2_IRQHandler(void)
{
    const uint32_t sr = TIM2->SR;
    if (sr & TIM_SR_CC2IF) {                            /* the PPS: a few instructions, then the sequencer */
        const uint32_t t = TIM2->CCR2;                  /* reading CCR2 clears CC2IF */
        if (sr & TIM_SR_CC2OF) {                        /* an earlier edge was overwritten */
            TIM2->SR = ~TIM_SR_CC2OF;
            gps_pps_lost_isr();
        }
        gps_pps_isr(t);
    }
    if (sr & TIM_SR_CC1IF) {
        TIM2->SR = ~TIM_SR_CC1IF;
        shot_tick();
    }
    __DSB();
}

/* ---- XFER_SENSE --------------------------------------------------------------------------------- */
uint32_t hw_xfer_start(uint16_t *buf, uint32_t n)
{
    hw_xfer_watch(false);
    xfer_n = n;
    adc_dummy(XFER_ADC);
    dma_start(DMA_XFER, MUX_XFER, REQ_ADC3, &XFER_ADC->DR, buf, n, false, false);
    XFER_ADC->ISR = ADC_ISR_OVR | ADC_ISR_EOC | ADC_ISR_EOS;
    adc_cr(XFER_ADC, ADC_CR_ADSTART);
    return sample_clock_start() + XFER_DELAY;
}

uint32_t hw_xfer_count(void)
{
    return xfer_n - DMA_XFER->CNDTR;
}

void hw_xfer_stop(void)
{
    sample_clock_stop();
    adc_stop(XFER_ADC);
    DMA_XFER->CCR = 0;
}

void hw_xfer_watch(bool on)
{
    EXTI->IMR1 &= ~EXTI_IMR1_IM0;
    EXTI->PR1 = EXTI_PR1_PIF0;
    if (!on) {
        pin_mode(XFER_PORT, XFER_PIN, 3u);              /* back to analog for ADC3 */
        return;
    }
    pin_mode(XFER_PORT, XFER_PIN, 0u);                  /* digital input */
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~SYSCFG_EXTICR1_EXTI0) | SYSCFG_EXTICR1_EXTI0_PB;
    EXTI->RTSR1 &= ~EXTI_RTSR1_RT0;
    EXTI->FTSR1 |= EXTI_FTSR1_FT0;
    EXTI->PR1 = EXTI_PR1_PIF0;
    EXTI->IMR1 |= EXTI_IMR1_IM0;
    if (!pin_read(XFER_PORT, XFER_PIN)) shot_fault(FAULT_XFER_DROP);   /* already low: no edge to see */
}

void EXTI0_IRQHandler(void)
{
    if (EXTI->PR1 & EXTI_PR1_PIF0) {
        EXTI->IMR1 &= ~EXTI_IMR1_IM0;       /* one trip voids the record: ignore a chattering contact */
        EXTI->PR1 = EXTI_PR1_PIF0;
        shot_fault(FAULT_XFER_DROP);
    }
    __DSB();
}

/* ---- I_SENSE ------------------------------------------------------------------------------------ */
void hw_isense_start(uint16_t awd_code)
{
    adc_stop(ISNS_ADC);
    for (uint32_t i = 0; i < ISNS_N; i++) isense_buf[i] = 0;
    dma_start(DMA_ISNS, MUX_ISNS, REQ_ADC2, &ISNS_ADC->DR, isense_buf, ISNS_N, false, true);
    ISNS_ADC->CFGR = ADC_CFGR_DMAEN | ADC_CFGR_DMACFG | ADC_CFGR_CONT | ADC_CFGR_OVRMOD
                     | ADC_CFGR_AWD1EN | ADC_CFGR_AWD1SGL | (ISNS_CHANNEL << ADC_CFGR_AWD1CH_Pos);
    ISNS_ADC->SQR1 = ISNS_CHANNEL << ADC_SQR1_SQ1_Pos;
    ISNS_ADC->TR1 = ((uint32_t)awd_code << ADC_TR1_HT1_Pos) | (ISNS_AWD_FILTER << ADC_TR1_AWDFILT_Pos);
    adc_dummy(ISNS_ADC);
    ISNS_ADC->ISR = ADC_ISR_AWD1 | ADC_ISR_OVR;
    ISNS_ADC->IER = ADC_IER_AWD1IE;
    adc_cr(ISNS_ADC, ADC_CR_ADSTART);
}

uint16_t hw_isense_mean(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < ISNS_N; i++) sum += isense_buf[i];
    return (uint16_t)((sum + ISNS_N / 2u) / ISNS_N);
}

void hw_isense_stop(void)
{
    adc_stop(ISNS_ADC);                 /* IER and CFGR can be written only with ADSTART = 0 */
    ISNS_ADC->IER = 0;
    ISNS_ADC->CFGR = 0;
    ISNS_ADC->ISR = ADC_ISR_AWD1 | ADC_ISR_OVR;
    DMA_ISNS->CCR = 0;
}

void ADC1_2_IRQHandler(void)
{
    if ((ISNS_ADC->IER & ADC_IER_AWD1IE) && (ISNS_ADC->ISR & ADC_ISR_AWD1)) {
        pin_low(GATE_PORT, GATE_PIN);                   /* first */
        pin_low(TUNE_PORT, TUNE_PIN);
        /* Only the flag: IER may be written only with ADSTART = 0 (RM0440 21.7.2). The sequencer, woken
         * now, stops ADC2; until then the interrupt can repeat every 4 conversions, harmlessly. */
        ISNS_ADC->ISR = ADC_ISR_AWD1;
        shot_fault(FAULT_OVERCURRENT);
    }
    __DSB();
}

/* ---- Gate guard --------------------------------------------------------------------------------- */
void hw_guard_arm(uint32_t ticks)
{
    uint32_t units = ticks / (TICK_HZ / 10000u);
    if (units > GUARD_CAP_10KHZ) units = GUARD_CAP_10KHZ;
    if (units < 2u) units = 2u;
    TIM7->CR1 = TIM_CR1_OPM | TIM_CR1_URS;
    TIM7->CNT = 0;
    TIM7->ARR = units - 1u;
    TIM7->SR = 0;
    TIM7->CR1 = TIM_CR1_OPM | TIM_CR1_URS | TIM_CR1_CEN;
}

void hw_guard_disarm(void)
{
    TIM7->CR1 = TIM_CR1_OPM | TIM_CR1_URS;
    TIM7->SR = 0;
}

void TIM7_DAC_IRQHandler(void)
{
    if (TIM7->SR & TIM_SR_UIF) {
        TIM7->SR = 0;
        pin_low(GATE_PORT, GATE_PIN);
        shot_fault(FAULT_GUARD);
    }
    __DSB();
}

/* ---- The record --------------------------------------------------------------------------------- */
uint32_t hw_record_start(uint16_t *buf, uint32_t n, uint32_t *edges, uint32_t n_edges)
{
    rec_n = n;
    edges_n = n_edges;
    adc_dummy(REC_ADC);
    dma_start(DMA_REC, MUX_REC, REQ_ADC1, &REC_ADC->DR, buf, n, false, false);
    REC_ADC->ISR = ADC_ISR_OVR | ADC_ISR_EOC | ADC_ISR_EOS;
    adc_cr(REC_ADC, ADC_CR_ADSTART);

    dma_start(DMA_EDGES, MUX_EDGES, REQ_TIM2_CH3, &TIM2->CCR3, edges, n_edges, true, false);
    TIM2->SR = ~(TIM_SR_CC3IF | TIM_SR_CC3OF);
    TIM2->DIER |= TIM_DIER_CC3DE;                       /* DMA first, so no edge is left unread */
    TIM2->CCER |= TIM_CCER_CC3E;

    return sample_clock_start() + REC_DELAY;
}

uint32_t hw_record_count(void)
{
    return rec_n - DMA_REC->CNDTR;
}

uint32_t hw_edges_count(void)
{
    return edges_n - DMA_EDGES->CNDTR;
}

bool hw_edges_overflow(void)
{
    return DMA_EDGES->CNDTR == 0u || (TIM2->SR & TIM_SR_CC3OF) != 0u;
}

void hw_record_stop(void)
{
    sample_clock_stop();
    adc_stop(REC_ADC);
    TIM2->DIER &= ~TIM_DIER_CC3DE;
    TIM2->CCER &= ~TIM_CCER_CC3E;
    DMA_REC->CCR = 0;
    DMA_EDGES->CCR = 0;
}

/* ---- Faults the hardware raises ------------------------------------------------------------------
 * Both handlers keep the order of every other path: GATE_CMD and TUNE low at once, RLY_CMD only after
 * at least 0.5 ms (the coil current is zero 59-71 us after gate-off; K1 must open cold). */

/* GATE_CMD and TUNE are already low: wait >= 0.5 ms, drop RLY_CMD, and leave the rest to the watchdog. */
static void __attribute__((noreturn)) hold_then_drop_rly(void)
{
    for (volatile uint32_t i = 0; i < 200000u; i++) {}  /* >= 1 ms at 170 MHz, longer on HSI16 */
    pin_low(RLY_PORT, RLY_PIN);
    for (;;) {}
}

void NMI_Handler(void)
{
    pin_low(GATE_PORT, GATE_PIN);                       /* first, whatever raised the NMI */
    pin_low(TUNE_PORT, TUNE_PIN);
    if (RCC->CIFR & RCC_CIFR_CSSF) {
        /* The HSE stopped and SYSCLK is now HSI16: the time base is wrong. The sequencer drops RLY_CMD
         * 85,000 counts after the gate - 5.3 ms at 16 MHz - and reports clock_fail. */
        RCC->CICR = RCC_CICR_CSSC;
        board_clock_lost();
        shot_fault(FAULT_CLOCK);
        __DSB();
        return;
    }
    /* Any other NMI (a flash double-ECC error, or SRAM parity) may stay asserted and re-enter forever,
     * so the sequencer cannot be trusted to run: finish here. */
    hold_then_drop_rly();
}

/* No stack, no C: a fault may have come from a broken stack. Pins and addresses are checked below. */
typedef char hardfault_pins_match_board_h[(GATE_PIN == 12u && TUNE_PIN == 10u && RLY_PIN == 2u) ? 1 : -1];
typedef char hardfault_gpioc_bsrr[(GPIOC_BASE + 0x18u == 0x48000818u) ? 1 : -1];
typedef char hardfault_gpiod_bsrr[(GPIOD_BASE + 0x18u == 0x48000C18u) ? 1 : -1];

void __attribute__((naked)) HardFault_Handler(void)
{
    __asm volatile(
        "ldr r0, =0x48000818 \n"    /* GPIOC->BSRR */
        "ldr r1, =0x14000000 \n"    /* reset PC12 (GATE_CMD) and PC10 (TUNE) */
        "str r1, [r0]        \n"
        "ldr r2, =200000     \n"    /* >= 1 ms at 170 MHz (3-4 cycles a turn), longer on HSI16 */
        "1: subs r2, r2, #1  \n"
        "bne 1b              \n"
        "ldr r0, =0x48000C18 \n"    /* GPIOD->BSRR */
        "ldr r1, =0x00040000 \n"    /* reset PD2 (RLY_CMD) */
        "str r1, [r0]        \n"
        "2: b 2b             \n"    /* the watchdog resets the MCU */
        ".ltorg              \n");  /* the constants, right here */
}
