// Common utility macros/inlines shared across all translation units.
#pragma once

#include <stdint.h>
#include "pico/stdlib.h"

static inline uint32_t board_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

static inline void mem_barrier(void) {
    __asm__ volatile("dmb" ::: "memory");
}

#define HALT() do { while (1) sleep_ms(1000); } while (0)
