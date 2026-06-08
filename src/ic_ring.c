#include "ic_ring.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <string.h>

// 32 KB per direction. Was 64 KB but the resulting 128 KB BSS footprint
// (×2 directions) starved heap and cJSON OOM'd on a 70 KB lab list. 32 KB
// still fits a 16-bit offset in the FIFO notification and is plenty for
// the few-KB TTS PCM chunks that phase-4 will stream.
#define IC_RING_SIZE  (32u * 1024u)
#define IC_RING_MASK  (IC_RING_SIZE - 1u)

// All messages are aligned to 4 bytes inside the ring so the next message's
// 16-bit header doesn't straddle a word boundary (Cortex-M33 supports
// unaligned halfword loads but it's still nicer to keep things aligned).
#define IC_ALIGN(n)   (((n) + 3u) & ~3u)
#define IC_HDR_BYTES  4u   // u16 type + u16 length

typedef struct {
    uint8_t  buf[IC_RING_SIZE];
    volatile uint32_t head;   // bytes written by producer (monotonic)
    volatile uint32_t tail;   // bytes consumed (monotonic)
} ic_ring_t;

// Index 0 = core0 → core1, index 1 = core1 → core0.
static ic_ring_t g_rings[2] __attribute__((aligned(4)));

// Per-core 1-deep peek cache. ic_peek_type pops the FIFO entry into here so
// the caller can inspect the type without consuming the payload from the
// ring. ic_try_recv drains the cache before touching the hardware FIFO.
// Indexed by get_core_num().
static struct {
    bool     valid;
    uint32_t entry;   // (type << 16) | offset, exactly as stored in the FIFO
} g_peek_cache[2];

static inline ic_ring_t *prod_ring(void) {
    return get_core_num() == 0 ? &g_rings[0] : &g_rings[1];
}

static inline ic_ring_t *cons_ring(void) {
    return get_core_num() == 0 ? &g_rings[1] : &g_rings[0];
}

void ic_init(void) {
    // Idempotent: only the first caller (across both cores) clears state.
    static volatile uint32_t inited = 0;
    if (inited) return;
    inited = 1;
    memset(g_rings, 0, sizeof g_rings);
    memset(g_peek_cache, 0, sizeof g_peek_cache);
}

// Wrap-safe memcpy from `src` into ring `r` starting at byte offset `off`.
static void ring_write(ic_ring_t *r, uint32_t off, const void *src, uint32_t n) {
    if (n == 0) return;
    uint32_t end = off + n;
    if (end <= IC_RING_SIZE) {
        memcpy(r->buf + off, src, n);
    } else {
        uint32_t first = IC_RING_SIZE - off;
        memcpy(r->buf + off, src, first);
        memcpy(r->buf, (const uint8_t *)src + first, n - first);
    }
}

// Wrap-safe memcpy from ring `r` starting at byte offset `off` into `dst`.
static void ring_read(ic_ring_t *r, uint32_t off, void *dst, uint32_t n) {
    if (n == 0) return;
    uint32_t end = off + n;
    if (end <= IC_RING_SIZE) {
        memcpy(dst, r->buf + off, n);
    } else {
        uint32_t first = IC_RING_SIZE - off;
        memcpy(dst, r->buf + off, first);
        memcpy((uint8_t *)dst + first, r->buf, n - first);
    }
}

int ic_send(ic_msg_t type, const void *payload, uint16_t length) {
    ic_ring_t *r = prod_ring();
    uint32_t need = IC_HDR_BYTES + IC_ALIGN(length);
    if (need > IC_RING_SIZE) return -1;   // physically can't fit

    // Spin until the consumer has freed enough space. Producer and consumer
    // are on different cores; this is fine as long as audio/network keep
    // making progress. (Same backpressure principle as TCP recv windowing.)
    while (1) {
        uint32_t free = IC_RING_SIZE - (r->head - r->tail);
        if (free >= need) break;
        tight_loop_contents();
    }

    uint32_t off = r->head & IC_RING_MASK;
    uint16_t hdr[2] = { (uint16_t)type, length };
    ring_write(r, off, hdr, IC_HDR_BYTES);
    ring_write(r, (off + IC_HDR_BYTES) & IC_RING_MASK, payload, length);

    // Publish: bytes-in-ring visible BEFORE the FIFO notification is observed.
    __sync_synchronize();
    r->head += need;

    // Notification carries (type, offset). Block briefly if FIFO (8 slots
    // per direction) is back-pressured by a slow consumer.
    uint32_t entry = ((uint32_t)(uint16_t)type << 16) | (off & 0xFFFFu);
    multicore_fifo_push_blocking(entry);
    return 0;
}

uint32_t ic_send_avail(void) {
    // FIFO check first: even if the ring has space, multicore_fifo_push_blocking
    // inside ic_send will stall if the 8-slot notification FIFO is full. That
    // would block core1's main loop and starve cyw43_arch_poll — the very
    // failure mode we added this for. Treat FIFO-full as "0 bytes available".
    if (!multicore_fifo_wready()) return 0;
    ic_ring_t *r = prod_ring();
    return IC_RING_SIZE - (r->head - r->tail);
}

int ic_peek_type(ic_msg_t *out_type) {
    int self = get_core_num();
    if (!g_peek_cache[self].valid) {
        if (!multicore_fifo_rvalid()) return -1;
        g_peek_cache[self].entry = multicore_fifo_pop_blocking();
        g_peek_cache[self].valid = true;
    }
    if (out_type) *out_type = (ic_msg_t)(uint16_t)(g_peek_cache[self].entry >> 16);
    return 0;
}

int ic_try_recv(ic_msg_t *out_type, void *payload_buf,
                uint16_t max_len, uint16_t *out_length) {
    int self = get_core_num();
    uint32_t entry;
    if (g_peek_cache[self].valid) {
        entry = g_peek_cache[self].entry;
        g_peek_cache[self].valid = false;
    } else {
        if (!multicore_fifo_rvalid()) return -1;
        entry = multicore_fifo_pop_blocking();   // already valid
    }
    // Ensure we observe the ring writes the producer made before pushing.
    __sync_synchronize();

    ic_ring_t *r = cons_ring();
    uint16_t  fifo_type = (uint16_t)(entry >> 16);
    uint32_t  off       = entry & 0xFFFFu;

    uint16_t hdr[2];
    ring_read(r, off, hdr, IC_HDR_BYTES);
    uint16_t hdr_type   = hdr[0];
    uint16_t hdr_length = hdr[1];

    // Sanity: FIFO type and ring header type must match. Mismatch means
    // someone scribbled the ring; bail rather than read garbage.
    if (hdr_type != fifo_type) return -1;

    if (out_type)   *out_type   = (ic_msg_t)hdr_type;
    if (out_length) *out_length = hdr_length;

    uint16_t copy_len = (hdr_length < max_len) ? hdr_length : max_len;
    if (copy_len > 0 && payload_buf) {
        ring_read(r, (off + IC_HDR_BYTES) & IC_RING_MASK, payload_buf, copy_len);
    }

    // Free the slot. We always consume the full advertised length even if
    // the caller's buffer was smaller — otherwise the ring would leak.
    uint32_t consumed = IC_HDR_BYTES + IC_ALIGN(hdr_length);
    __sync_synchronize();
    r->tail += consumed;
    return 0;
}

int ic_recv(ic_msg_t *out_type, void *payload_buf,
            uint16_t max_len, uint16_t *out_length) {
    while (ic_try_recv(out_type, payload_buf, max_len, out_length) != 0) {
        tight_loop_contents();
    }
    return 0;
}
