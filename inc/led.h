// Non-blocking LED blink state machine for CYW43 on-board LED.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start a blink pattern: toggles `count` times at `interval_ms`.
void led_blink_start(uint32_t interval_ms, int count);

// Drive the blink state machine — call from the main loop.
void led_blink_loop(void);

// 1 Hz heartbeat — suppressed while a blink pattern is active.
void led_heartbeat_loop(void);
