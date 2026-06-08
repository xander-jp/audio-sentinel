// Inter-core message bus.
//
// Design:
//   - One ring buffer per direction (core0→core1, core1→core0), shared SRAM.
//   - Each message in the ring is laid out as [u16 type][u16 length][payload].
//   - The hardware multicore FIFO carries a 32-bit notification per message:
//         high 16 bits = type
//         low  16 bits = byte offset within the producer's ring
//   - Producer writes the payload first, then pushes the FIFO entry.
//   - Consumer pops a FIFO entry, reads payload at the embedded offset, then
//     advances the ring's tail.
//
// SPSC per direction: each ring has exactly one producer (own core) and one
// consumer (other core), so head/tail need only volatile accesses + DMB
// barriers (no spinlock).
//
// Ring size is 64 KB which fits a 16-bit offset comfortably and is enough
// for a couple of TTS PCM chunks in flight.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum {
    IC_MSG_NONE = 0,

    // --- Boot-time handshake (S0 → S1 transition) ---
    IC_MSG_NET_READY      = 0x10,  // core1 → core0: WiFi/DHCP/DNS up
    IC_MSG_AUDIO_READY    = 0x11,  // core0 → core1: audio_init + boot beep done

    // --- Core0 → Core1 control messages ---
    IC_MSG_BTN_X          = 0x20,  // request lab fetch
    IC_MSG_BTN_Y          = 0x21,  // return to button view
    IC_MSG_TIMELINE_START = 0x22,  // payload = operator_id string (no NUL)
    IC_MSG_TIMELINE_STOP  = 0x23,
    IC_MSG_TTS_REQUEST    = 0x24,  // payload = UTF-8 text (no NUL)
    IC_MSG_TTS_PLAYED     = 0x25,  // core0 finished playback → pop queue
    IC_MSG_RADIO_START    = 0x26,  // start internet-radio stream (no payload)
    IC_MSG_RADIO_STOP     = 0x27,  // stop in-flight radio stream

    // --- Linephone (walkie-talkie) core0 → core1 ---
    IC_MSG_LINEPHONE_OPUS_PKT = 0x28,  // mic-encoded opus packet (accumulate)
    IC_MSG_LINEPHONE_POST_END = 0x29,  // capture done → base64+POST accumulator
    IC_MSG_LINEPHONE_START    = 0x2A,  // payload = operator_id (enter mode)
    IC_MSG_LINEPHONE_STOP     = 0x2B,  // leave linephone mode

    // --- Core1 → Core0 status / data messages ---
    IC_MSG_LABS_READY     = 0x30,  // labs JSON parsed, g_labs[] populated
    IC_MSG_TTS_PCM_CHUNK  = 0x31,  // payload = raw int16-LE PCM bytes
    IC_MSG_TTS_END        = 0x32,  // no more PCM for the in-flight TTS
    IC_MSG_TTS_OPUS_PKT   = 0x33,  // payload = raw opus packet (decode on core0)
} ic_msg_t;

// Initialize both rings. Safe to call from each core; first call wins.
void ic_init(void);

// Send a message. Blocks while the producer ring lacks space or the hardware
// FIFO is full (i.e. consumer is behind). Length 0 + payload NULL is allowed
// (signal-only). Returns 0 on success, -1 if length exceeds ring capacity.
int  ic_send(ic_msg_t type, const void *payload, uint16_t length);

// Non-blocking receive. Returns 0 with type/length filled if a message was
// pending, -1 if none. payload_buf may be NULL if max_len is 0.
int  ic_try_recv(ic_msg_t *out_type, void *payload_buf,
                 uint16_t max_len, uint16_t *out_length);

// Blocking receive — spins until a message arrives. Same out semantics as
// ic_try_recv.
int  ic_recv(ic_msg_t *out_type, void *payload_buf,
             uint16_t max_len, uint16_t *out_length);

// Non-destructive peek: returns 0 with out_type filled if a message is at
// the head of the FIFO, -1 if empty. Repeated peeks return the same type
// until the next ic_try_recv / ic_recv consumes it. Internally the FIFO
// entry is moved into a per-core 1-deep cache on first peek, so consume
// must happen via ic_try_recv (which checks the cache before the FIFO).
int  ic_peek_type(ic_msg_t *out_type);

// Producer-side: number of bytes that would fit into the local→remote ring
// right now WITHOUT blocking. Returns 0 if either the ring is full or the
// hardware notification FIFO (8 slots) is full — both would cause ic_send
// to block. Callers can use this to skip a ship instead of stalling cyw43
// poll on a slow consumer.
uint32_t ic_send_avail(void);
