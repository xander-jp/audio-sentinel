#include "tts.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include "pico/cyw43_arch.h"

#include "audio.h"
#include "http.h"
#include "ic_ring.h"
#include "queue.h"
#include "gui.h"      // g_tts_play_active
#include "opus_stream.h"

#define TTS_PATH  "/api/tts/generate_stream"

//=============================================================================
// Shared state (externed in tts.h)
//=============================================================================
uint8_t           g_core0_pcm_pending[TTS_PCM_PENDING_CAP];
volatile size_t   g_core0_pcm_pending_len = 0;
volatile bool     g_core0_tts_stream_done = false;
volatile size_t   g_tts_play_pos = 0;

//=============================================================================
// Private state
//=============================================================================
static size_t g_tts_pad_remaining = 0;
// core1-private guard: set true after we ship IC_MSG_TTS_END so the
// forward pump doesn't re-send. Reset on each new playback start.
static bool   g_core1_tts_end_sent = false;

void tts_send_end_if_needed(void) {
    if (!g_core1_tts_end_sent) {
        ic_send(IC_MSG_TTS_END, NULL, 0);
        g_core1_tts_end_sent = true;
    }
}

#define TTS_COMPACT_THRESHOLD  2048

//=============================================================================
// Playback arm (called from http.c after TTS headers parse)
//=============================================================================
void tts_start_playback(void) {
    if (!g_https_headers_done) {
        printf("[tts] start_playback called before headers_done — ignoring\n");
        return;
    }
    audio_set_sample_rate(g_tts_sample_rate);
    audio_stream_reset();
    // Match decoder rate to the I2S rate the server negotiated. Without this
    // the decoder stayed at 16 kHz while TTS responses arrived at 24 kHz, so
    // the decoder emitted 320 samples per 20 ms but I2S clocked them out at
    // 24 kHz → 1.5× too fast.
    if (g_https_body_opus) opus_stream_set_rate(g_tts_sample_rate);
    opus_stream_reset();
    g_tts_play_pos      = 0;
    g_tts_pad_remaining = 0;
    g_core0_pcm_pending_len  = 0;
    g_core0_tts_stream_done  = false;
    g_core1_tts_end_sent     = false;
    mem_barrier();
    g_tts_play_active = true;
    size_t body_have = g_https_resp_len - g_https_body_start;
    printf("[tts] playback armed: body@%u resp_len=%u initial_body=%u Hz=%u\n",
           (unsigned)g_https_body_start, (unsigned)g_https_resp_len,
           (unsigned)body_have, (unsigned)g_tts_sample_rate);
}

//=============================================================================
// JSON escape + TTS request builder
//=============================================================================
static int json_escape(char *dst, size_t dst_sz, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        char tmp[8];
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (c < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    esc = tmp;
                }
                break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= dst_sz) return -1;
            memcpy(dst + o, esc, n);
            o += n;
        } else {
            if (o + 1 >= dst_sz) return -1;
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

int tts_kick_request(const char *message, const char *gender,
                     const ip_addr_t *ip, const char *host,
                     const char *device_id, bool opus_enabled,
                     const char *operator_id) {
    g_https_mode = HM_TTS;

    char body[TTS_MSG_LEN * 2 + 192];
    char escaped[TTS_MSG_LEN * 2];
    if (json_escape(escaped, sizeof(escaped), message) < 0) {
        printf("[tts] escape overflow\n");
        return -1;
    }
    const char *g = (gender && *gender) ? gender : "male";
    // Optional "operator_id" segment — only emitted when the caller
    // hands us a non-empty id. Server uses it to lock TTS routing to
    // that operator and stop the radio operator from hijacking the
    // response when one is registered for this device.
    char op_seg[MAX_LAB_ID_LEN + 32] = "";
    if (operator_id && operator_id[0]) {
        snprintf(op_seg, sizeof(op_seg),
                 "\"operator_id\":\"%s\",", operator_id);
    }
    int blen = snprintf(body, sizeof(body),
                        "{%s\"gender\":\"%s\",\"style\":\"neutral\","
                        "\"out_lang\":\"ja\",\"opus\":%s,\"text\":\"%s\"}",
                        op_seg, g, opus_enabled ? "true" : "false", escaped);
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        printf("[tts] body overflow\n");
        return -1;
    }
    printf("[tts] POST body (%d B): %s\n", blen, body);

    // Header line is built at runtime now (used to be a string-literal concat
    // with a build-time DEVICE_ID macro). info/timeline already do the same.
    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "X-Device-Id: %s\r\n"
             "X-Sample-Rate: 24000\r\n",
             device_id ? device_id : "");
    if (http_build_request("POST", TTS_PATH, hdr,
                            "application/json",
                            body, (size_t)blen) != 0) return -1;
    return http_request_start(ip, host);
}

//=============================================================================
// Response buffer compaction (core1-only).
//
// All g_https_resp access (recv_cb append, chunked decode, forward_pump read,
// compact memmove) now happens sequentially on core1's main loop — core0
// only sees opus packets via the IC ring. So no lock is needed here.
// Kept under cyw43_arch_lwip_begin/end purely to serialize against the
// lwIP callback chain on the same core, matching the old behaviour.
//=============================================================================
static void tts_resp_compact(void) {
    if (g_tts_play_pos < TTS_COMPACT_THRESHOLD) return;
    cyw43_arch_lwip_begin();
    http_resp_compact_locked();
    cyw43_arch_lwip_end();
}

//=============================================================================
// Core0: audio ring drain.
//
// Both opus and raw-PCM paths feed the audio ring via core1 → IC ring.
// Opus packets are decoded inline in handle_core0_notify (main.c) and the
// resulting PCM is written straight to the audio ring; raw PCM is staged
// in g_core0_pcm_pending and drained here. The pump itself only handles
// the pending → ring copy, underrun recovery, and end-of-stream pad.
//=============================================================================
bool tts_play_pump(void) {
    (void)audio_stream_buffered();

    if (g_tts_play_active) audio_stream_underrun_recover();

    // RAW PCM path: drain pending (filled by core1 via IC_MSG_TTS_PCM_CHUNK).
    if (g_core0_pcm_pending_len >= 2) {
        size_t samples = g_core0_pcm_pending_len / 2;
        const int16_t *src = (const int16_t *)g_core0_pcm_pending;
        size_t w = audio_stream_write_mono16(src, samples);
        if (w > 0) {
            size_t consumed = w * 2;
            size_t remain   = g_core0_pcm_pending_len - consumed;
            if (remain > 0) memmove(g_core0_pcm_pending,
                                    g_core0_pcm_pending + consumed, remain);
            g_core0_pcm_pending_len = remain;
        }
    }

    // End-of-stream detection. Both paths converge on IC_MSG_TTS_END for
    // "no more source data coming" (core1 ships it after the chunked
    // terminator or recv FIN). They diverge on source_drained:
    //  - opus path: packets are decoded straight into the audio ring as
    //    they arrive, so once TTS_END is received there is no further
    //    consumer state to wait on — the ring drain happens naturally
    //    as the pad zeros are appended behind any remaining samples.
    //  - raw PCM path: g_core0_pcm_pending must be flushed into the ring
    //    before we declare the source drained.
    bool stream_done    = g_core0_tts_stream_done;
    bool source_drained = g_https_body_opus
                             ? true
                             : (g_core0_pcm_pending_len < 2);

    if (stream_done && source_drained && g_tts_pad_remaining == 0
        && g_tts_play_active) {
        // Must wipe the FULL DMA ring; otherwise ENDLESS DMA keeps reading
        // the stale PCM tail and we hear a periodic "chi-chi-chi" loop at
        // ring-lap frequency.
        g_tts_pad_remaining = (size_t)BUF_FRAMES + 256u;
    }

    if (g_tts_pad_remaining > 0) {
        static const int16_t zero_block[128] = {0};
        size_t want = g_tts_pad_remaining < 128 ? g_tts_pad_remaining : 128;
        size_t w = audio_stream_write_mono16(zero_block, want);
        g_tts_pad_remaining -= w;
    }

    if (stream_done && source_drained && g_tts_pad_remaining == 0
        && g_tts_play_active) {
        printf("[tts] playback done (pending drained, pad written)\n");
        g_tts_play_active        = false;
        g_core0_tts_stream_done  = false;
        ic_send(IC_MSG_TTS_PLAYED, NULL, 0);
        return false;
    }
    return g_tts_play_active;
}

//=============================================================================
// Core1: chunked decoder + IC forwarder for both opus packets and raw PCM.
//
// Opus path: extract each [u16 BE length L][L bytes] frame from the chunked-
// decoded body and ship the raw opus packet via IC_MSG_TTS_OPUS_PKT. core0
// decodes inline in handle_core0_notify and writes PCM straight to the
// audio ring. We never touch the decoder on core1, so cyw43_arch_poll
// never has to wait on opus_decode.
//
// Raw PCM path: copy a chunk of the body and ship as IC_MSG_TTS_PCM_CHUNK
// for core0's pending buffer (unchanged from the pre-opus design).
//
// All g_https_resp access is core1-only — recv_cb, chunked decode, this
// pump's reads, and compact memmove all run sequentially in core1's main
// loop. compact still uses cyw43_arch_lwip_begin/end to serialize against
// lwIP callbacks on the same core.
//
// IC ships are non-blocking: ic_send_avail() covers both ring space AND
// the 8-slot notification FIFO so we never stall multicore_fifo_push_blocking
// and starve cyw43_arch_poll. If the consumer is slow we break out and the
// next pump call retries. Back-pressure then cascades:
//     audio ring >75% → core0 stops IC drain → IC ring fills →
//     this pump skips → g_https_resp fills → recv_cb ERR_MEM →
//     TCP window closes → server throttles.
//=============================================================================
#define TTS_FWD_MAX_FRAMES_PER_PASS  4

bool tts_forward_pump(void) {
    if (!g_tts_play_active)       return false;
    if (g_core1_tts_end_sent)     return false;
    if (!g_https_headers_done)    return true;

    if (g_https_chunked) http_chunked_decode_in_place();

    size_t body_have = g_https_chunked
                         ? g_chunked_write_pos
                         : (g_https_resp_len - g_https_body_start);

    if (g_https_body_opus) {
        const uint8_t *body = (const uint8_t *)g_https_resp + g_https_body_start;
        int shipped = 0;
        while (shipped < TTS_FWD_MAX_FRAMES_PER_PASS
               && g_tts_play_pos + 2u <= body_have) {
            uint16_t L = ((uint16_t)body[g_tts_play_pos]     << 8)
                       |  (uint16_t)body[g_tts_play_pos + 1];
            if (L == 0) {
                // Zero-length opus packet — server inserts these as silence /
                // sync markers (observed in radio startup). Skip and keep
                // scanning; do NOT abort the stream.
                g_tts_play_pos += 2u;
                continue;
            }
            if (L > OPUS_STREAM_MAX_PACKET) {
                printf("[tts/opus] bad packet length=%u at pos=%u — abort\n",
                       (unsigned)L, (unsigned)g_tts_play_pos);
                g_tts_play_pos = body_have;
                break;
            }
            if (g_tts_play_pos + 2u + L > body_have) {
                // Packet header says L bytes follow but only body_have-pos-2
                // are present. When stream_done is true the body is final
                // (chunked CHUNK_DONE or transport closed) and body_have
                // won't grow — silent-breaking would wedge the pump
                // forever and audio underruns continuously. Dump the
                // surrounding bytes (one-shot) and force-abort so playback
                // finalizes the partial audio we already shipped.
                bool stream_final = (g_https_state == HC_IDLE)
                                    || (g_https_chunked
                                          ? (g_chunked_state == CHUNK_DONE)
                                          : false);
                if (stream_final) {
                    static bool dumped = false;
                    if (!dumped) {
                        dumped = true;
                        size_t avail = body_have - g_tts_play_pos;
                        size_t dump_n = avail < 16 ? avail : 16;
                        printf("[tts/opus] framing wedge at pos=%u "
                               "L=%u body_have=%u remaining=%u dump:",
                               (unsigned)g_tts_play_pos, (unsigned)L,
                               (unsigned)body_have, (unsigned)avail);
                        for (size_t i = 0; i < dump_n; i++) {
                            printf(" %02x", body[g_tts_play_pos + i]);
                        }
                        printf("\n");
                    }
                    g_tts_play_pos = body_have;   // drain remaining body
                    break;
                }
                break;   // still streaming — wait for more bytes
            }

            // ic_send_avail() == 0 means either the ring is full or the
            // notification FIFO is full. Either way ic_send would block,
            // which would freeze cyw43_arch_poll. Break and retry.
            // 4 = IC header bytes; the payload is rounded up to 4-byte
            // alignment inside the ring.
            uint32_t need = 4u + ((L + 3u) & ~3u);
            if (ic_send_avail() < need) {
                // Throttled diagnostic: print why we're stuck so we can
                // tell IC-ring back-pressure (consumer slow) apart from
                // FIFO back-pressure (8-slot notif FIFO full). Throttled
                // to 500 ms so a sustained wedge doesn't flood UART.
                static uint32_t last_bp_log = 0;
                uint32_t now_bp = board_millis();
                if ((now_bp - last_bp_log) >= 500) {
                    last_bp_log = now_bp;
                    printf("[tts/opus] ship-block need=%u avail=%u pos=%u/%u "
                           "shipped_this_pass=%d\n",
                           (unsigned)need, (unsigned)ic_send_avail(),
                           (unsigned)g_tts_play_pos, (unsigned)body_have,
                           shipped);
                }
                break;
            }

            ic_send(IC_MSG_TTS_OPUS_PKT,
                    body + g_tts_play_pos + 2u, L);
            g_tts_play_pos += 2u + L;
            shipped++;
        }
        if (shipped > 0) tts_resp_compact();

        static uint32_t last_log_opus = 0;
        uint32_t now = board_millis();
        if (shipped > 0 && (now - last_log_opus) >= 250) {
            last_log_opus = now;
            printf("[tts/opus] fwd pos=%u/%u (+%d pkt)\n",
                   (unsigned)g_tts_play_pos, (unsigned)body_have, shipped);
        }
    } else {
        // ---- Raw PCM path ----
        size_t bytes_left = body_have - g_tts_play_pos;
        if (bytes_left >= 2) {
            size_t n = bytes_left;
            if (n > TTS_FWD_CHUNK_SIZE) n = TTS_FWD_CHUNK_SIZE;
            n &= ~1u;

            uint32_t need = 4u + ((n + 3u) & ~3u);
            if (ic_send_avail() >= need) {
                const uint8_t *src = (const uint8_t *)g_https_resp
                                     + g_https_body_start + g_tts_play_pos;
                ic_send(IC_MSG_TTS_PCM_CHUNK, src, (uint16_t)n);
                g_tts_play_pos += n;
                tts_resp_compact();

                static uint32_t last_log_pcm = 0;
                uint32_t now = board_millis();
                if ((now - last_log_pcm) >= 250) {
                    last_log_pcm = now;
                    printf("[tts/pcm] fwd pos=%u/%u (+%u bytes)\n",
                           (unsigned)g_tts_play_pos, (unsigned)body_have,
                           (unsigned)n);
                }
            }
        }
    }

    // HC_IDLE means the transport is gone (TCP FIN or RADIO_STOP →
    // http_cleanup). Treat that as "no more body bytes coming" even
    // for chunked responses — radio streams never reach CHUNK_DONE
    // because the user-side stop tears down the pcb mid-stream, so
    // without this check the forward pump would wait forever and
    // never ship TTS_END, leaving g_tts_play_active stuck on.
    bool stream_done    = (g_https_state == HC_IDLE)
                          || (g_https_chunked
                                ? (g_chunked_state == CHUNK_DONE)
                                : false);
    bool source_drained = (body_have - g_tts_play_pos) < 2;

    if (stream_done && source_drained) {
        printf("[tts/%s] fwd done: %u body bytes consumed\n",
               g_https_body_opus ? "opus" : "pcm",
               (unsigned)g_tts_play_pos);
        ic_send(IC_MSG_TTS_END, NULL, 0);
        g_core1_tts_end_sent = true;
        return false;
    }
    return true;
}
