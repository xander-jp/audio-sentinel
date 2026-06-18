// Opus packet decoder for streaming TTS (16 kHz mono, 20 ms frames).
//
// Wire format on the HTTPS body (server-side: request JSON has "opus": true):
//
//     for each packet:
//         [u16 big-endian length L][L bytes of opus packet]
//
// Decoded PCM is shipped to core0 over the IC ring as IC_MSG_TTS_PCM_CHUNK,
// identical to the raw-PCM path — so audio.c / tts_play_pump are unchanged.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// PCM-output buffer size used per decoded frame. Sized for 20 ms @ 24 kHz =
// 480 samples — the worst case among rates we negotiate with the server (TTS
// uses 24 kHz, radio uses 16 kHz). opus_decode returns the actual sample
// count, which is ≤ this max for any rate ≤ 24 kHz at 20 ms.
#define OPUS_STREAM_FRAME_SAMPLES  480
// Defensive upper bound on a single opus packet. RFC 6716 allows up to
// 1275 bytes per packet; at 16-24 kbps / 20 ms we expect ~40-60 B average, so
// anything beyond this is almost certainly a framing desync.
#define OPUS_STREAM_MAX_PACKET     1500

// One-shot decoder bring-up at the default rate (16 kHz). Idempotent.
// Returns 0 on success, -1 on failure. Prefer opus_stream_set_rate() when
// you know the playback rate up front.
int  opus_stream_init(void);

// Recreate the decoder at `hz` if the current decoder is at a different
// rate (or doesn't exist yet). Cheap no-op if the rate is unchanged.
// Server's X-Sample-Rate / X-Opus-Sample-Rate drives this — TTS uses 24 kHz,
// the radio broadcast uses 16 kHz, so we negotiate per-stream.
int  opus_stream_set_rate(uint32_t hz);

// Forget all stateful decoder context (PLC history, etc.). Call at the
// start of each new TTS playback.
void opus_stream_reset(void);

// Destroy the decoder entirely, returning its ~18 KB of heap. The decoder is
// re-created lazily on the next opus_stream_decode_frame(). Used by the mic
// push-to-talk path so the encoder and decoder never coexist on the heap.
void opus_stream_free(void);

// Decode a single opus packet. Returns the number of decoded samples
// (mono int16), or a negative opus error code on failure.
int  opus_stream_decode_frame(const uint8_t *packet, size_t packet_len,
                              int16_t *pcm_out, int pcm_max);
