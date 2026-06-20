//=============================================================================
// hc_sr04.c — thin C glue over the PIO range finder. All timing is in PIO
// (src/hc_sr04.pio); this file just starts pings and converts the raw
// countdown the SM reports back into centimetres.
//=============================================================================
#include "hc_sr04.h"
#include "hc_sr04.pio.h"   // generated from hc_sr04.pio by pico_generate_pio_header

// Speed of sound 343 m/s → 0.0343 cm/µs, halved for the round trip:
//   cm = us * 0.0343 / 2 = us * 0.01715
#define HC_SR04_CM_PER_US 0.01715f

void hc_sr04_init(hc_sr04_t *s, PIO pio, uint trig_pin, uint echo_pin) {
    s->pio = pio;
    s->sm  = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &hc_sr04_program);
    hc_sr04_program_init(pio, s->sm, offset, trig_pin, echo_pin);
}

void hc_sr04_trigger(hc_sr04_t *s) {
    // The SM is parked on `pull block`; this hands it the timeout and lets it
    // run one full measurement. Non-blocking unless a prior ping is still
    // unconsumed in the TX FIFO (it normally isn't, given the read-then-trigger
    // cadence).
    pio_sm_put(s->pio, s->sm, HC_SR04_TIMEOUT_US);
}

bool hc_sr04_read(hc_sr04_t *s, float *cm) {
    if (pio_sm_is_rx_fifo_empty(s->pio, s->sm))
        return false;

    uint32_t remaining = pio_sm_get(s->pio, s->sm);
    if (remaining == 0) {           // sentinel: ECHO never rose (no signal)
        *cm = -1.0f;
        return true;
    }
    if (remaining == 1) {           // sentinel: ECHO stuck high (measure timeout)
        *cm = -2.0f;
        return true;
    }
    uint32_t elapsed_us = HC_SR04_TIMEOUT_US - remaining;
    *cm = (float)elapsed_us * HC_SR04_CM_PER_US;
    return true;
}
