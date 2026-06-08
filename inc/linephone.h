// Walkie-talkie / "linephone": opus packets streamed from core0 accumulate
// here, get base64-encoded, and shipped as a POST to /api/device/linephone/.
//
// Wire format inside the accumulator (matches the playback path's wire
// format on the way out):
//
//     for each opus packet:
//         [u16 BE length L][L bytes of opus packet]
//
// The accumulator runs on core1 (it's fed by IC ring messages from core0
// and the POST itself happens on core1's HTTPS state machine).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Max accumulated raw opus bytes (length-prefixed framing included). 12 KB
// at 8 kbps voice + 20 ms framing ≈ 11 seconds — plenty for push-to-talk.
#define LP_OPUS_CAP   (12 * 1024)

// Debug echo: include the device's own POSTs in GET responses (loopback).
// Turn off in production so a device only hears OTHER devices' utterances.
#ifndef LP_ECHO_MODE
#define LP_ECHO_MODE 1
#endif

// Latest stream position acknowledged by the server. The next GET sends
// ?start=<this>; on a successful reply the server returns header
// "X-Linephone-Head: N" and we advance to N so subsequent GETs only see
// newer utterances. Lives in BSS so it resets to 0 on reboot — server
// interprets start=0 as "skip past, start from current head", so the device
// won't re-hear historical traffic after a power cycle.
extern volatile int64_t g_lp_start_pos;

// Parse the X-Linephone-Head response header (if present) and advance the
// cursor. Called from http.c's HM_LINEPHONE_GET completion path.
void lp_handle_get_head_header(const char *resp, size_t header_len);

// Reset to empty (call when starting a new capture session).
void lp_accum_reset(void);

// Append one opus packet to the accumulator with [u16 BE len][bytes] framing.
// Drops the packet if there isn't enough room.
void lp_accum_append(const uint8_t *opus_pkt, uint16_t len);

// Number of raw bytes currently buffered (post-framing).
size_t lp_accum_len(void);

// Build "POST /api/device/linephone/" with the accumulator base64-encoded as
// the body and kick off the HTTPS request. operator_id (required, non-empty)
// pins the request to the active linephone operator — without it the proxy
// → tunnel_client routing falls back to the device's default operator, which
// may be a different role (radio/gameserver) registered for this device.
// Send-only (fire-and-forget): the response is not routed to the audio
// pipeline. Returns 0 on success, -1 on failure.
int  lp_kick_post(const char *device_id, const char *operator_id);

// Build "GET /api/device/linephone/?device_id=…&operator_id=…&start=N" and
// kick off the HTTPS request. operator_id is required (same reason as POST).
// Response body, if audio/opus, is routed to the same playback path as TTS.
// 404 → IDLE. Returns 0 on success, -1 on failure.
int  lp_kick_get(const char *device_id, const char *operator_id);
