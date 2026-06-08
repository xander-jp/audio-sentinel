#include "opus_stream.h"

#include <stdio.h>
#include <string.h>

#include "opus.h"

// Default rate for opus_stream_init() — chosen low so the boot-time
// decoder allocation matches the radio path (which dominates uptime).
// Single-utterance TTS pumps will call opus_stream_set_rate(24000) before
// playback starts and the decoder gets recreated.
#define OPUS_SR_DEFAULT  16000

static OpusDecoder *g_decoder    = NULL;
static uint32_t     g_decoder_sr = 0;

int opus_stream_set_rate(uint32_t hz) {
    if (g_decoder && g_decoder_sr == hz) return 0;
    if (g_decoder) {
        opus_decoder_destroy(g_decoder);
        g_decoder    = NULL;
        g_decoder_sr = 0;
    }
    int err = OPUS_OK;
    g_decoder = opus_decoder_create((opus_int32)hz, 1, &err);
    if (!g_decoder || err != OPUS_OK) {
        printf("[opus] decoder_create(%u Hz) failed err=%d\n", (unsigned)hz, err);
        g_decoder = NULL;
        return -1;
    }
    g_decoder_sr = hz;
    // [stackdiag] exact heap cost of this decoder instance (mono). This is
    // what opus_decoder_create() malloc()'d — compare against the encoder's
    // and the free-heap probe to see how tight the newlib heap is.
    printf("[opus] decoder ready (%uHz mono, fixed-point) heap=%d B\n",
           (unsigned)hz, opus_decoder_get_size(1));
    return 0;
}

int opus_stream_init(void) {
    if (g_decoder) return 0;
    return opus_stream_set_rate(OPUS_SR_DEFAULT);
}

void opus_stream_reset(void) {
    if (g_decoder) {
        opus_decoder_ctl(g_decoder, OPUS_RESET_STATE);
    }
}

int opus_stream_decode_frame(const uint8_t *packet, size_t packet_len,
                              int16_t *pcm_out, int pcm_max) {
    if (!g_decoder && opus_stream_init() != 0) return -1;
    int n = opus_decode(g_decoder, packet, (int32_t)packet_len,
                        pcm_out, pcm_max, 0);
    if (n < 0) {
        printf("[opus] decode err=%d (pkt=%u)\n", n, (unsigned)packet_len);
    }
    return n;
}
