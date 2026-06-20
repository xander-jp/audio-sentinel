#ifndef HC_SR04_H
#define HC_SR04_H
//=============================================================================
// hc_sr04.h — non-blocking HC-SR04 ultrasonic range finder driven entirely by
// PIO (see src/hc_sr04.pio). The CPU only fires a ping and polls a FIFO; the
// trigger pulse and echo-width timing happen on the state machine, so there is
// NO busy-wait on the main loop (the old blocking driver could stall up to
// 60 ms waiting on echo — fatal for the opus realtime path).
//
// Usage (event-loop style):
//     hc_sr04_t sonar;
//     hc_sr04_init(&sonar, pio1, 6, 7);     // TRIG=GP6, ECHO=GP7 (ECHO divided)
//     ...
//     hc_sr04_trigger(&sonar);              // fire one measurement
//     ...
//     float cm;
//     if (hc_sr04_read(&sonar, &cm)) {      // non-blocking; true when ready
//         if (cm < 0) puts("out of range");
//         else        printf("%.1f cm\n", cm);
//     }
//
// Cadence: keep at least one full timeout (~30 ms) between triggers, or simply
// re-trigger only after hc_sr04_read() returns true.
//=============================================================================
#include "hardware/pio.h"

#define HC_SR04_TIMEOUT_US 30000u   // ~5 m of round-trip range before giving up

typedef struct {
    PIO  pio;
    uint sm;
} hc_sr04_t;

// Claim a free SM on `pio`, load the program, configure TRIG/ECHO pins.
void hc_sr04_init(hc_sr04_t *s, PIO pio, uint trig_pin, uint echo_pin);

// Fire one measurement. Non-blocking — the PIO does the trigger and timing.
void hc_sr04_trigger(hc_sr04_t *s);

// Non-blocking. Returns false if no result is ready yet. When it returns true,
// *cm holds the distance in centimetres, or a diagnostic negative code:
//   -1.0 → ECHO never rose (no signal: trigger/power/wiring/divider-too-low)
//   -2.0 → ECHO stuck high (divider-too-high / floating / short)
bool hc_sr04_read(hc_sr04_t *s, float *cm);

#endif // HC_SR04_H
