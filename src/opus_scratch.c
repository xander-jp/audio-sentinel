/* opus_scratch.c — BSS-backed pseudostack for libopus. See inc/opus_scratch.h.
 *
 * libopus (NONTHREADSAFE_PSEUDOSTACK) asks for GLOBAL_STACK_SIZE bytes of
 * scratch once, via opus_alloc_scratch(), and reuses that region for every
 * subsequent codec call (global_stack is reset to the base each top-level
 * opus_encode/opus_decode, so usage never accumulates across calls). We hand
 * back a fixed BSS buffer instead of malloc() so the codec scratch never
 * draws from the newlib heap that cyw43 / lwIP / mbedtls live in.
 *
 * Size budget: the old VLA build measured a *hardware-stack* peak of
 * ~18,912 B for encode (that figure also included C call frames, register
 * spills and IRQ nesting). The pseudostack holds only the codec ALLOC()
 * arrays — a strict subset — so the real high-water for our mono / wideband /
 * 20 ms / complexity-0 config is comfortably under that. We size this buffer
 * at 24 KB: margin over the (inflated) 18,912 B figure while leaving the
 * newlib heap large enough for the mbedtls TLS context (IN 16 KB + OUT 4 KB
 * content buffers, ~20 KB+ at handshake). An earlier 40 KB buffer made the
 * device boot-OOM in the first HTTPS GET because BSS stole that RAM from the
 * heap. The trailing canary plus opus's ENABLE_HARDENING turn any future
 * regression (raised complexity, stereo, higher bitrate) into a loud,
 * detectable failure instead of silent BSS corruption.
 */
#include "opus_scratch.h"

#include <stdint.h>

/* Trip value written just past the usable region. 8-byte aligned buffer so
 * opus's ALIGN() never needs more than the natural alignment of its types. */
#define OPUS_SCRATCH_CANARY 0xC5A5C3A9u

static uint8_t g_opus_scratch[OPUS_SCRATCH_BYTES + sizeof(uint32_t)]
    __attribute__((aligned(8)));

static volatile uint32_t *const g_opus_scratch_canary =
    (volatile uint32_t *)(g_opus_scratch + OPUS_SCRATCH_BYTES);

void *opus_alloc_scratch(size_t size)
{
    /* size is always GLOBAL_STACK_SIZE (== OPUS_SCRATCH_BYTES); the region is
     * fixed, so the argument is advisory only. */
    (void)size;
    *g_opus_scratch_canary = OPUS_SCRATCH_CANARY;
    return g_opus_scratch;
}

int opus_scratch_canary_ok(void)
{
    return *g_opus_scratch_canary == OPUS_SCRATCH_CANARY;
}
