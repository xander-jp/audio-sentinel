// TTS streaming pipeline: request building, PCM forwarding (core1),
// and audio ring draining (core0).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TTS_FWD_CHUNK_SIZE   1024
#define TTS_PCM_PENDING_CAP  8192
// Raw HTTP body bytes that must accumulate (~chunk-header overhead is
// negligible) before audio playback is armed. Arming earlier means the DMA
// ring is empty when the I2S engine starts reading and the first ~ring-
// worth of audio is silence + underrun spam. 8 KB ≈ 170 ms @ 24 kHz mono
// int16 — matches the 32 KB DMA ring (one full lap of cushion).
#define TTS_PREBUFFER_BYTES  8192

// Pending PCM staging buffer — core0 drains via tts_play_pump().
// Written by handle_core0_notify (IC_MSG_TTS_PCM_CHUNK).
extern uint8_t           g_core0_pcm_pending[TTS_PCM_PENDING_CAP];
extern volatile size_t   g_core0_pcm_pending_len;
extern volatile bool     g_core0_tts_stream_done;

// Arm audio streaming playback (called from http.c after headers parse).
void tts_start_playback(void);

// Send IC_MSG_TTS_END if not already sent (error recovery path).
void tts_send_end_if_needed(void);

// Build and send a TTS POST request via the HTTPS client.
// ip/host are the resolved API endpoint. device_id is the BLE-provisioned
// per-device UUID consumed by the API for auth (sent as X-Device-Id).
// gender may be NULL/"" — defaults to "male".
#include "lwip/ip_addr.h"
// operator_id (optional, NULL or "" to omit): when present, included in
// the request body as "operator_id" — the server uses it to target the
// TTS at a specific operator instead of falling back to whatever default
// it would otherwise pick (which, with a radio operator in the list,
// hijacks the response into a radio stream).
int tts_kick_request(const char *message, const char *gender,
                     const ip_addr_t *ip, const char *host,
                     const char *device_id, bool opus_enabled,
                     const char *operator_id);

// Core0: drain g_core0_pcm_pending into the audio ring.
// Returns true while playback is in flight.
bool tts_play_pump(void);

// Core1: forward PCM from g_https_resp to core0 via IC ring.
// Returns true while forwarding is in flight.
bool tts_forward_pump(void);
