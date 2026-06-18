/* opus_scratch.h — pseudostack backing store for libopus on RP2350.
 *
 * libopus is built here with NONTHREADSAFE_PSEUDOSTACK (see
 * cmake.rp2350/CMakeLists.txt), so the SILK/CELT codec scratch arrays that
 * used to be VLAs on the caller's hardware stack now live in one global
 * pseudostack obtained, once, from opus_alloc_scratch(). We override that
 * allocator (OVERRIDE_OPUS_ALLOC_SCRATCH on the opus target) so the scratch
 * comes from a fixed BSS buffer in this firmware rather than malloc() — it
 * must NOT compete with the newlib heap that cyw43 / lwIP / mbedtls share,
 * which is the very collision the old big-stack layout was fighting.
 *
 * This header is force-included into every opus translation unit (-include)
 * to give them the prototype os_support.h no longer provides once
 * OVERRIDE_OPUS_ALLOC_SCRATCH is set. The definition lives in
 * src/opus_scratch.c and is linked into the final image.
 *
 * SAFETY: a single global pseudostack is only sound because both opus entry
 * points — opus_encode() (mic_ship_frame) and opus_decode()
 * (opus_stream_decode_frame) — run sequentially on core0's main loop, never
 * nested and never from an IRQ. See src/opus_scratch.c for the size budget
 * and the end canary.
 */
#ifndef AUDIO_SENTINEL_OPUS_SCRATCH_H
#define AUDIO_SENTINEL_OPUS_SCRATCH_H

#include <stddef.h>

/* Bytes of pseudostack handed to libopus. Must equal GLOBAL_STACK_SIZE
 * passed to the opus target. The codec's real high-water for our config
 * (mono, wideband, 20 ms, complexity 0 encode / SILK WB decode) is well
 * under this; see the canary note in src/opus_scratch.c. */
#define OPUS_SCRATCH_BYTES (24u * 1024u)

/* Backing allocator for opus's NONTHREADSAFE_PSEUDOSTACK. Called exactly
 * once, on the first opus_encode()/opus_decode(), with size ==
 * GLOBAL_STACK_SIZE. Returns the fixed BSS buffer; the size is ignored. */
void *opus_alloc_scratch(size_t size);

/* Returns non-zero while the trailing canary past the pseudostack is intact.
 * A zero return means the codec overran OPUS_SCRATCH_BYTES and corrupted
 * adjacent BSS — treat as fatal. Cheap enough to call once per frame. */
int opus_scratch_canary_ok(void);

#endif /* AUDIO_SENTINEL_OPUS_SCRATCH_H */
