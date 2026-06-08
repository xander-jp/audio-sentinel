#include "linephone.h"
#include "http.h"
#include "wifi.h"      // g_api_ip
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Public proxy whitelist (server side) only allows /api/device/* prefix to
// reach the linephone backend; a bare /api/linephone/ would be 404'd at
// the proxy before it reaches the upstream.
#define LP_PATH  "/api/device/linephone/"

// === Accumulator ============================================================
static uint8_t g_lp_buf[LP_OPUS_CAP];
static size_t  g_lp_len = 0;

volatile int64_t g_lp_start_pos = 0;

void lp_accum_reset(void) {
    g_lp_len = 0;
}

void lp_accum_append(const uint8_t *opus_pkt, uint16_t len) {
    if (len == 0) return;
    if (g_lp_len + 2u + (size_t)len > LP_OPUS_CAP) {
        static uint32_t last_drop_log = 0;
        uint32_t now = board_millis();
        if ((now - last_drop_log) >= 500) {
            last_drop_log = now;
            printf("[lp/accum] cap reached (%u/%u) — dropping pkt len=%u\n",
                   (unsigned)g_lp_len, (unsigned)LP_OPUS_CAP, (unsigned)len);
        }
        return;
    }
    g_lp_buf[g_lp_len++] = (uint8_t)(len >> 8);
    g_lp_buf[g_lp_len++] = (uint8_t)(len & 0xff);
    memcpy(g_lp_buf + g_lp_len, opus_pkt, len);
    g_lp_len += len;
}

size_t lp_accum_len(void) { return g_lp_len; }

// === Base64 encoder =========================================================
static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encodes src[0..n) into dst, returning the number of base64 chars written
// (excluding any trailing NUL — none is written). Caller must ensure dst has
// at least ((n + 2) / 3) * 4 bytes.
static size_t b64_encode(const uint8_t *src, size_t n, char *dst) {
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)src[i] << 16)
                   | ((uint32_t)src[i + 1] << 8)
                   |  (uint32_t)src[i + 2];
        dst[o++] = b64_alphabet[(v >> 18) & 0x3F];
        dst[o++] = b64_alphabet[(v >> 12) & 0x3F];
        dst[o++] = b64_alphabet[(v >> 6)  & 0x3F];
        dst[o++] = b64_alphabet[ v        & 0x3F];
        i += 3;
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        dst[o++] = b64_alphabet[(v >> 18) & 0x3F];
        dst[o++] = b64_alphabet[(v >> 12) & 0x3F];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[o++] = b64_alphabet[(v >> 18) & 0x3F];
        dst[o++] = b64_alphabet[(v >> 12) & 0x3F];
        dst[o++] = b64_alphabet[(v >> 6)  & 0x3F];
        dst[o++] = '=';
    }
    return o;
}

// === POST kick (send speech) ===============================================
//
// Body JSON shape (server-agreed):
//   {"device_id":"...","sample_rate":16000,"codec":"opus","audio":"<base64>"}
//
// The audio field contains the entire length-prefixed opus stream, base64-
// encoded. Server decodes by reading [u16 BE len][bytes] frames identical to
// how this device decodes the playback path.
//
// Send-only: server's response (likely a small JSON status) is not routed
// to the audio pipeline — reception is GET /api/device/linephone/'s job. We build
// the body straight into a scratch and hand it to http_build_request which
// copies into g_https_req. REQ_BUF_SIZE was bumped so the headers + body
// fit even at the LP_OPUS_CAP ceiling (~16 KB base64 + ~256 B headers).
int lp_kick_post(const char *device_id, const char *operator_id) {
    static char body[LP_OPUS_CAP * 4 / 3 + 256];   // base64 + JSON wrapper
    size_t b64_max = (g_lp_len + 2) / 3 * 4;
    if (sizeof(body) < b64_max + 200) {
        printf("[lp] static body too small (%u < %u)\n",
               (unsigned)sizeof(body), (unsigned)(b64_max + 200));
        return -1;
    }

    // Build the body in two passes: prefix + base64 + suffix. operator_id
    // is required so the proxy → tunnel_client routing pins to the active
    // linephone operator instead of falling back to the device default
    // (which might be a radio/gameserver operator).
    int prefix_n = snprintf(body, sizeof(body),
                            "{\"device_id\":\"%s\","
                            "\"operator_id\":\"%s\","
                            "\"sample_rate\":16000,"
                            "\"codec\":\"opus\","
                            "\"audio\":\"",
                            device_id ? device_id : "",
                            operator_id ? operator_id : "");
    if (prefix_n < 0 || (size_t)prefix_n + b64_max + 3 >= sizeof(body)) {
        printf("[lp] body overflow at prefix\n");
        return -1;
    }
    size_t b64_n = b64_encode(g_lp_buf, g_lp_len, body + prefix_n);
    size_t pos   = (size_t)prefix_n + b64_n;
    if (pos + 3 >= sizeof(body)) {
        printf("[lp] body overflow after base64\n");
        return -1;
    }
    body[pos++] = '"';
    body[pos++] = '}';
    body[pos]   = '\0';

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "X-Device-Id: %s\r\n",
             device_id ? device_id : "");

    g_https_mode = HM_LINEPHONE_POST;
    printf("[lp/post] %s (opus=%u B → b64=%u B, total body=%u B)\n",
           LP_PATH, (unsigned)g_lp_len, (unsigned)b64_n, (unsigned)pos);

    if (http_build_request("POST", LP_PATH, hdr,
                            "application/json", body, pos) != 0) return -1;
    // Body has been copied into g_https_req — safe to clear the accumulator
    // so the next push-to-talk session starts empty.
    lp_accum_reset();
    return http_request_start(&g_api_ip, g_https_host);
}

// === GET kick (poll for queued speech) =====================================
//
// Idempotent poll. Response:
//   200 audio/opus chunked → handed off to the TTS playback pipeline
//   404                    → nothing pending; just IDLE
//
// Device id rides as both a query parameter and the X-Device-Id header for
// parity with the other endpoints (the server may route on either).
int lp_kick_get(const char *device_id, const char *operator_id) {
    // Path layout: ?device_id=<id>&operator_id=<op>&start=<latest-pos>[&echo=1]
    // - operator_id pins the proxy → tunnel_client routing to the active
    //   linephone operator (same role as the body field in POST)
    // - start tells the server "I've already heard everything up to N"
    // - echo=1 (debug only) lets the device hear its own POSTs too
    char path[224];
    int n = snprintf(path, sizeof(path),
                     "%s?device_id=%s&operator_id=%s&start=%lld%s",
                     LP_PATH,
                     device_id ? device_id : "",
                     operator_id ? operator_id : "",
                     (long long)g_lp_start_pos,
                     LP_ECHO_MODE ? "&echo=1" : "");
    if (n < 0 || (size_t)n >= sizeof(path)) {
        printf("[lp/get] path overflow\n");
        return -1;
    }
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "X-Device-Id: %s\r\n",
             device_id ? device_id : "");

    g_https_mode = HM_LINEPHONE_GET;
    printf("[lp/get] %s\n", path);
    if (http_build_request("GET", path, hdr, NULL, NULL, 0) != 0) return -1;
    return http_request_start(&g_api_ip, g_https_host);
}

void lp_handle_get_head_header(const char *resp, size_t header_len) {
    const char *p = http_find_header(resp, header_len, "X-Linephone-Head");
    if (!p) return;
    // atoll stops at the first non-digit (handles trailing \r\n).
    long long v = atoll(p);
    if (v > g_lp_start_pos) {
        printf("[lp/get] cursor %lld → %lld\n",
               (long long)g_lp_start_pos, v);
        g_lp_start_pos = (int64_t)v;
    }
}
