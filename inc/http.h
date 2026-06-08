// HTTPS client (altcp_tls).
//
// Layered as a thin transport: owns the connection state machine, request
// buffer, response buffer, header/body parsing, and chunked transfer-encoding
// decoder. Application-level concerns (URL paths, JSON parsing, TTS audio
// pumping) live in main.c and reach into the response via the extern globals
// declared below.
//
// Application hook: tts_start_playback() is implemented by main.c and called
// directly from http_check_complete after headers parse in HM_TTS mode.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/altcp.h"
#include "lwip/ip_addr.h"

#define HTTPS_PORT          443
// Response body sizing:
//   - HM_INFO uses /api/tunnel/info?t=simple, which omits recent_additions
//     and returns just operators[]/lab_ids → a few KB at most.
//   - HM_TIMELINE returns a JSON array of timeline items and can grow
//     several tens of KB; 16 KB was too small here, 64 KB has headroom.
//   - HM_TTS is chunked, and chunk-bursts from the API are followed by
//     1–2 s server pauses while the next chunk is generated. We must hold
//     enough decoded PCM to drain across those pauses or the audio ring
//     underruns; 128 KB buys ~2.6 s @ 24 kHz mono int16. Bigger than this
//     starts squeezing the newlib heap that mbedtls' TLS handshake uses
//     (~+188 KB BSS total has been observed to OOM altcp_tls_new).
// recv_cb has ERR_MEM backpressure if the buffer ever fills (real for
// TTS — see above), so the bound is hard-safe.
#define RESP_BUF_SIZE       (128 * 1024)
// Big enough for HTTP POST headers + a few KB of UTF-8 message body, PLUS
// the linephone POST which carries a base64-encoded opus stream (up to ~16 KB
// of base64 payload + ~256 B headers). 20 KB covers the LP_OPUS_CAP ceiling
// with a comfortable margin.
#define REQ_BUF_SIZE        (20 * 1024)
#define HTTPS_TIMEOUT_MS    20000

// HTTP log verbosity: 0 = errors only, 1 = compact per-transfer lines,
// 2 = full trace (header dump, ACK/poll/write chatter). Runtime-tunable.
// Default 1: the level-2 lines run 1-2 KB per transfer, and pico-sdk stdio
// serializes cores on one mutex — at 115200 baud a header dump holds it
// for ~90 ms, stalling core0's mic pump past the ring cushion (measured
// robo-voice cause). Errors always print regardless of level.
extern volatile uint8_t g_http_log_level;
#define HTTP_LOGI(...) do { if (g_http_log_level >= 1) printf(__VA_ARGS__); } while (0)
#define HTTP_LOGV(...) do { if (g_http_log_level >= 2) printf(__VA_ARGS__); } while (0)
// altcp_poll interval is in 500ms units; 4 = 2s
#define HTTPS_POLL_INTERVAL 4

typedef enum {
    HC_IDLE,
    HC_CONNECTING,
    HC_REQUESTING,
    HC_DONE_OK,
    HC_DONE_ERR,
} https_state_t;

// What kind of request is currently in flight (or just finished).
typedef enum {
    HM_INFO,            // GET /api/tunnel/info  → lab list
    HM_TIMELINE,        // GET /api/qa/timeline?...  → JSON
    HM_TTS,             // POST /api/tts/generate_stream  → raw PCM / opus body
    HM_LINEPHONE_POST,  // POST /api/device/linephone/  → fire-and-forget (upload speech)
    HM_LINEPHONE_GET,   // GET  /api/device/linephone/  → 200 audio/opus (reply) or 404
} https_mode_t;

// Chunked transfer-encoding decoder state (used when the server returns
// Transfer-Encoding: chunked, which uvicorn's StreamingResponse does).
// Decoded body bytes are compacted in-place inside g_https_resp[body_start...]
// so the downstream PCM pump can treat g_chunked_write_pos as the "decoded
// body length" without caring about framing.
typedef enum {
    CHUNK_NEED_SIZE,     // accumulating hex digits of chunk size
    CHUNK_SIZE_SAW_CR,   // got \r, expecting \n
    CHUNK_DATA,          // copying chunk data bytes
    CHUNK_DATA_SAW_CR,   // got \r after data, expecting \n
    CHUNK_DONE,          // 0-size chunk seen → stream finished
} chunk_state_t;

// === Transport globals ====================================================
// These are owned by http.c (it does all the writes during connect/recv) but
// readable by application pumps (TTS forwarder, JSON parsers, status logs).
extern volatile https_state_t g_https_state;
extern volatile https_mode_t  g_https_mode;
extern uint32_t               g_https_state_at;
extern char                   g_https_host[128];
extern char                   g_https_resp[RESP_BUF_SIZE];
extern volatile size_t        g_https_resp_len;
extern volatile bool          g_https_headers_done;
extern volatile size_t        g_https_body_start;
extern volatile int           g_https_content_len;   // -1 = unknown
extern volatile uint32_t      g_tts_sample_rate;     // parsed from X-Sample-Rate

extern volatile uint32_t g_recv_bytes;       // TCP payload bytes since last log
extern volatile uint32_t g_recv_pkts;        // recv callbacks since last log

extern volatile size_t g_tts_play_pos;       // bytes forwarded via FIFO (core1)
extern bool          g_https_chunked;
// True when the response Content-Type is "audio/opus" — i.e. the server
// honored our "opus":true request. Drives the opus decode path on core0
// and the eager chunked decoding in http_check_complete.
extern volatile bool          g_https_body_opus;
extern volatile size_t        g_chunked_read_pos;   // offset (from body_start) into raw chunked stream
extern volatile size_t        g_chunked_write_pos;  // offset where decoded PCM bytes have been written
extern volatile chunk_state_t g_chunked_state;

// === Public API ===========================================================
// Set the connection state and stamp g_https_state_at with the current ms.
void http_set_state(https_state_t s);

// Tear down the in-flight pcb (if any). Safe to call from any state.
void http_cleanup(void);

// Build "METHOD path HTTP/1.1\r\n..." into the internal request buffer.
// extra_headers (each line ending in \r\n) is appended verbatim. If body
// is non-NULL, Content-Type and Content-Length headers are added
// automatically and the body is appended after the header terminator.
// Returns 0 on success, -1 if the request would exceed REQ_BUF_SIZE.
int http_build_request(const char *method,
                       const char *path,
                       const char *extra_headers,
                       const char *content_type,
                       const char *body,
                       size_t      body_len);

// Reset response/decoder state, create a fresh TLS pcb, and kick off a
// connect to ip:HTTPS_PORT with SNI=host. The previously-built request body
// is flushed once the TCP/TLS handshake completes. Returns 0 on success,
// -1 if pcb allocation or altcp_connect failed.
int http_request_start(const ip_addr_t *ip, const char *host);

// Decode chunked transfer-encoding in-place inside g_https_resp[body_start...].
// Reads from g_chunked_read_pos, writes decoded PCM bytes to g_chunked_write_pos.
// Both offsets are relative to body_start. Resumes where the previous call
// left off; called from the TTS forward pump every iteration.
void http_chunked_decode_in_place(void);

// Locate the body separator ("\r\n\r\n") and return a pointer past it, or
// NULL if not yet present in the buffer.
const char *http_find_body(const char *resp, size_t len);

// Case-insensitive header lookup; returns a pointer to the value (past ':'
// and any leading whitespace) or NULL if not found.
const char *http_find_header(const char *resp, size_t len, const char *name);

// Pull X-Sample-Rate and Transfer-Encoding from the response headers into
// g_tts_sample_rate / g_https_chunked. Called automatically from
// http_check_complete for HM_TTS; also available for the FIN fallback path.
// Kick off a GET /api/tunnel/info request.
int http_kick_info(const char *device_id);

// Kick off a GET /api/qa/timeline request.
int http_kick_timeline(const char *device_id, const char *operator_id,
                       int64_t last_publish_id);

// Timeline last-seen publish ID (owned by http.c, used by main.c for
// kick_timeline and status display).
extern int64_t g_last_publish_id;

void http_apply_tts_headers(void);

// Handle HTTPS response completion: parse body, dispatch by mode,
// then cleanup + set state to IDLE.
void https_handle_done(void);

// Compact already-consumed body bytes from g_https_resp, rebasing offsets.
// Caller must hold the lwIP/async_context lock (recv_cb path does).
void http_resp_compact_locked(void);

// === Application hooks (implemented by main.c) ============================
// Arm audio streaming playback. Called from http_check_complete after
// http_apply_tts_headers() when HM_TTS headers are parsed.
void tts_start_playback(void);
