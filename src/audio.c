#include "audio.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "i2s.pio.h"
#include <stdio.h>
#include <string.h>

// RP2350 (Pico 2 W): CYW43 PIO-SPI driver は pio2 SM0 を奪う。
// 同じ PIO の別 SM を pio_claim_unused_sm で動的に取ることで衝突回避。
#define AUDIO_PIO       pio2
#define PIN_DIN         26
#define PIN_BCK         27
#define PIN_LRCK        28

#define DEFAULT_SAMPLE_RATE 24000

// I2S frame is now 2 uint32 words per stereo frame (one per channel, each
// carrying the 16-bit sample with 1 delay bit + 15 padding bits as required
// by Philips I2S). So a ring of N stereo frames needs 2*N words.
//
// 32KB ring (BUF_FRAMES stereo frames × 2 words × 4 bytes) → ~170ms @ 24kHz.
// BUF_FRAMES is exposed via audio.h so streaming clients can size their
// end-of-stream silence pad to a full ring lap.
// Buffer must be aligned to its size for the DMA ring wrap trick.
// RP2350 channel_config_set_ring upper bound is 15 bits (32KB) in practice;
// 16 bits silently broke the wrap and produced full silence in earlier tests.
#define BUF_WORDS       (BUF_FRAMES * 2)        // 8192 uint32 words
#define BUF_BYTES       (BUF_WORDS * 4)         // 32 KB
#define BUF_RING_BITS   15                      // log2(BUF_BYTES)

static int g_audio_sm = -1;
static int g_dma_ch   = -1;
static volatile bool g_audio_paused = false;

// 32-sample sine table (16-bit signed)
static const int16_t sine32[32] = {
        0,  1561,  3061,  4444,  5657,  6652,  7392,  7846,
     8000,  7846,  7392,  6652,  5657,  4444,  3061,  1561,
        0, -1561, -3061, -4444, -5657, -6652, -7392, -7846,
    -8000, -7846, -7392, -6652, -5657, -4444, -3061, -1561,
};

static uint32_t __attribute__((aligned(BUF_BYTES))) g_audio_buf[BUF_WORDS];

// Ring index in WORDS (0..BUF_WORDS-1) where the next producer word will
// land. We DELIBERATELY do not keep an absolute counter: on RP2350 the DMA's
// TRANS_COUNT register splits into MODE[31:28] + COUNT[27:0]; with ENDLESS
// mode (which is what we want — DMA must never stop for audio) the COUNT
// field stays put, so an absolute "consumed" derived from TRANS_COUNT lies.
// Instead we maintain an ABSOLUTE word counter on each side and derive the
// DMA-side counter from the hardware read_addr + a software wrap counter.
// Going absolute lets us tell "ring full" apart from "writer was passed by
// DMA (underrun)" — pure modulo math conflates the two and a single late
// chunk freezes the producer forever, looping the last 85ms of audio.
static volatile uint32_t g_write_abs   = 0;   // total words ever written
static volatile bool     g_audio_ready = false;

// Wrap-tracker state for converting modulo read_addr → absolute consumed.
// Only the producer/consumer poller (single thread on core1) should call
// dma_consumed_abs(); the wrap count would race if shared.
static uint32_t g_last_ring_pos = 0;
static uint32_t g_consumed_abs  = 0;

bool audio_is_ready(void) { return g_audio_ready; }

// Current ring word index the DMA is reading (0..BUF_WORDS-1).
static inline uint32_t dma_read_ring_pos(void) {
    uintptr_t base = (uintptr_t)g_audio_buf;
    uintptr_t ra   = (uintptr_t)dma_hw->ch[g_dma_ch].read_addr;
    uint32_t off_words = (uint32_t)((ra - base) >> 2);
    return off_words & (BUF_WORDS - 1);
}

// Absolute word count DMA has consumed since startup. Must be called more
// often than one ring lap (~85ms @ 24kHz) so the wrap detection doesn't
// miss a full revolution. Both audio_stream_write_mono16 and
// audio_stream_buffered call this, and they fire many times per ring lap.
static inline uint32_t dma_consumed_abs(void) {
    uint32_t now = dma_read_ring_pos();
    uint32_t delta = (now - g_last_ring_pos) & (BUF_WORDS - 1);
    g_consumed_abs += delta;
    g_last_ring_pos = now;
    return g_consumed_abs;
}

void audio_set_sample_rate(uint32_t hz) {
    if (hz < 8000)  hz = 8000;
    if (hz > 96000) hz = 96000;
    // PIO frame is 128 cycles (64 BCK × 2 PIO cycles/BCK).
    // BCK = sample_rate × 64 (32 BCK per channel × 2 channels).
    float sys_hz = (float)clock_get_hz(clk_sys);
    float div    = sys_hz / ((float)hz * 128.0f);
    // RP2350 PIO CLKDIV is only re-sampled on SM enable or pio_sm_clkdiv_restart.
    // Writing CLKDIV on a running SM keeps the OLD divider — boot beep ran at
    // 24 kHz, TTS streams 16 kHz, and without this disable/re-enable the I2S
    // clock stayed at the 24 kHz boot value → 16 kHz audio came out 1.5× too
    // fast. (A previous attempt at pio_sm_clkdiv_restart on a hot SM stalled
    // DMA; flipping enable explicitly is the safe re-load path.)
    // Preserve pause state across a sample-rate change: if we're paused
    // (B-button held for push-to-talk), don't re-enable the SM here — the
    // matching audio_resume() will turn it back on. Otherwise an upstream
    // tts_start_playback() arriving mid-pause would silently override the
    // pause and start emitting audio while the mic is recording.
    bool was_running = g_audio_ready && !g_audio_paused;
    if (was_running) pio_sm_set_enabled(AUDIO_PIO, g_audio_sm, false);
    pio_sm_set_clkdiv(AUDIO_PIO, g_audio_sm, div);
    pio_sm_clkdiv_restart(AUDIO_PIO, g_audio_sm);
    if (was_running) pio_sm_set_enabled(AUDIO_PIO, g_audio_sm, true);
    printf("[audio] clk_sys=%.0fHz target=%uHz div=%.3f → PIO=%.0fHz BCK=%.0fHz frame=%.0fHz\n",
           (double)sys_hz, (unsigned)hz, (double)div,
           (double)(sys_hz / div),
           (double)(sys_hz / div / 2.0f),
           (double)(sys_hz / div / 128.0f));
}

void audio_init(void) {
    printf("[audio] init begin\n");

    memset(g_audio_buf, 0, sizeof g_audio_buf);
    hard_assert(((uintptr_t)g_audio_buf & (BUF_BYTES - 1)) == 0);

    g_audio_sm = pio_claim_unused_sm(AUDIO_PIO, true);
    uint offset = pio_add_program(AUDIO_PIO, &i2s_out_program);
    i2s_out_program_init(AUDIO_PIO, g_audio_sm, offset, PIN_DIN, PIN_BCK);

    // Strengthen the I2S clock/data edges. Pico's 4mA default produces slow
    // rise/fall at 1.5MHz BCK — the PCM5101A PLL can struggle to lock on
    // soft edges, manifesting as harmonic distortion / "raspy" sine.
    gpio_set_drive_strength(PIN_DIN,  GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PIN_BCK,  GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PIN_LRCK, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(PIN_DIN,  GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_BCK,  GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIN_LRCK, GPIO_SLEW_RATE_FAST);

    audio_set_sample_rate(DEFAULT_SAMPLE_RATE);

    g_dma_ch = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(g_dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(AUDIO_PIO, g_audio_sm, true));
    channel_config_set_ring(&cfg, false, BUF_RING_BITS);

    dma_channel_configure(
        g_dma_ch, &cfg,
        &AUDIO_PIO->txf[g_audio_sm],
        g_audio_buf,
        // ENDLESS mode (RP2350): DMA never stops, ring wraps via read_addr.
        // SDK helper sets MODE=0xF correctly — passing 0xFFFFFFFF directly
        // would silently mean the same thing on RP2350 but breaks any reader
        // of TRANS_COUNT, so we use the documented helper.
        dma_encode_endless_transfer_count(),
        true           // START
    );

    pio_sm_set_enabled(AUDIO_PIO, g_audio_sm, true);

    __sync_synchronize();
    g_audio_ready = true;

    printf("[audio] ready: pio=%p sm=%d dma_ch=%d pins(DIN/BCK/LRCK)=GP%d/%d/%d buf=%p (%d frames / %d words, %d bytes)\n",
           (void*)AUDIO_PIO, g_audio_sm, g_dma_ch,
           PIN_DIN, PIN_BCK, PIN_LRCK,
           (void*)g_audio_buf, BUF_FRAMES, BUF_WORDS, BUF_BYTES);
}

// I2S Philips pack: bit 31 = 0 (delay), bits 30..15 = 16-bit sample MSB
// first, bits 14..0 = padding. Two words per stereo frame (R then L).
static inline uint32_t pack_i2s_word(int16_t sample) {
    return ((uint32_t)(uint16_t)sample) << 15;
}

void audio_play_sine(void) {
    if (!g_audio_ready) {
        printf("[audio] play_sine: NOT READY\n");
        return;
    }
    for (int f = 0; f < BUF_FRAMES; f++) {
        uint32_t word = pack_i2s_word(sine32[f % 32]);
        g_audio_buf[f * 2]     = word;   // right channel
        g_audio_buf[f * 2 + 1] = word;   // left channel (mono → L=R)
    }
    __sync_synchronize();
    printf("[audio] PLAY sine\n");
}

void audio_test_stream_sine(void) {
    if (!g_audio_ready) {
        printf("[audio] test_stream_sine: NOT READY\n");
        return;
    }
    audio_set_sample_rate(DEFAULT_SAMPLE_RATE);
    audio_stream_reset();
    const int BLOCKS = 48;   // 48 * 256 / 24000 ≈ 0.5s of audio
    printf("[audio] test_stream_sine: begin (%d blocks x 256 samples @ %dHz, ~%dms)\n",
           BLOCKS, DEFAULT_SAMPLE_RATE, BLOCKS * 256 * 1000 / DEFAULT_SAMPLE_RATE);
    uint32_t t_start = to_ms_since_boot(get_absolute_time());
    int16_t tmp[256];
    for (int t = 0; t < BLOCKS; t++) {
        for (int i = 0; i < 256; i++) {
            tmp[i] = sine32[(t * 256 + i) & 31];
        }
        size_t off = 0;
        int spin = 0;
        while (off < 256) {
            size_t w = audio_stream_write_mono16(tmp + off, 256 - off);
            off += w;
            if (w == 0) {
                spin++;
                if (spin > 500) {
                    // 500ms with zero progress: ring stuck (DMA not draining).
                    printf("[audio] test_stream_sine: STUCK at t=%d off=%u "
                           "buffered=%u consumed=%u write_abs=%u — abort\n",
                           t, (unsigned)off,
                           (unsigned)audio_stream_buffered(),
                           (unsigned)dma_consumed_abs(),
                           (unsigned)g_write_abs);
                    return;
                }
                sleep_ms(1);
            }
        }
        if ((t & 7) == 0) {
            printf("[audio] test_stream_sine: t=%d/%d buffered=%u\n",
                   t, BLOCKS, (unsigned)audio_stream_buffered());
        }
    }
    // Overwrite the entire ring with silence so the DMA doesn't wrap around
    // and re-play the trailing sine forever. Need > BUF_FRAMES samples; with
    // 256-sample blocks that's ceil(BUF_FRAMES/256)+1 blocks.
    int16_t zero_block[256] = {0};
    const int wipe_blocks = (BUF_FRAMES / 256) + 1;
    for (int t = 0; t < wipe_blocks; t++) {
        size_t off = 0;
        int spin = 0;
        while (off < 256) {
            size_t w = audio_stream_write_mono16(zero_block + off, 256 - off);
            off += w;
            if (w == 0) {
                if (++spin > 500) break;
                sleep_ms(1);
            }
        }
    }
    uint32_t t_end = to_ms_since_boot(get_absolute_time());
    printf("[audio] test_stream_sine: done in %ums\n", (unsigned)(t_end - t_start));
}

// Tick-driven variant. The blocking version above interleaves write +
// sleep_ms(1) when the ring is full; here we just return RUNNING on a no-
// progress tick and let the cooperative event loop come back. "Stuck"
// detection now uses a millis timestamp instead of a spin counter so it
// works regardless of how often the caller ticks us.
typedef enum {
    TEST_PHASE_SINE = 0,
    TEST_PHASE_SILENCE,
    TEST_PHASE_DONE,
} test_phase_t;

#define TEST_SINE_BLOCKS     48                       // 48 × 256 / 24000 ≈ 0.5s sine
#define TEST_SILENCE_BLOCKS  ((BUF_FRAMES / 256) + 1) // > BUF_FRAMES / 256 → guarantees full wipe
#define TEST_BLOCK_SAMPLES   256
#define TEST_STUCK_MS        500

static struct {
    test_phase_t phase;
    int          block;
    int          off;
    uint32_t     t_last_progress_ms;
    uint32_t     t_start_ms;
} g_test = { .phase = TEST_PHASE_DONE };

void audio_test_stream_sine_begin(void) {
    if (!g_audio_ready) {
        printf("[audio] test_stream_sine_begin: NOT READY\n");
        g_test.phase = TEST_PHASE_DONE;
        return;
    }
    audio_set_sample_rate(DEFAULT_SAMPLE_RATE);
    audio_stream_reset();
    g_test.phase = TEST_PHASE_SINE;
    g_test.block = 0;
    g_test.off   = 0;
    g_test.t_start_ms         = to_ms_since_boot(get_absolute_time());
    g_test.t_last_progress_ms = g_test.t_start_ms;
    printf("[audio] test_stream_sine_begin: %d sine + %d silence blocks @ %dHz\n",
           TEST_SINE_BLOCKS, TEST_SILENCE_BLOCKS, DEFAULT_SAMPLE_RATE);
}

audio_test_status_t audio_test_stream_sine_tick(void) {
    if (g_test.phase == TEST_PHASE_DONE) return AUDIO_TEST_DONE;
    if (!g_audio_ready) { g_test.phase = TEST_PHASE_DONE; return AUDIO_TEST_DONE; }

    int16_t tmp[TEST_BLOCK_SAMPLES];
    if (g_test.phase == TEST_PHASE_SINE) {
        for (int i = 0; i < TEST_BLOCK_SAMPLES; i++) {
            tmp[i] = sine32[(g_test.block * TEST_BLOCK_SAMPLES + i) & 31];
        }
    } else {
        memset(tmp, 0, sizeof tmp);
    }

    size_t w = audio_stream_write_mono16(tmp + g_test.off,
                                         (size_t)(TEST_BLOCK_SAMPLES - g_test.off));
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (w > 0) {
        g_test.off += (int)w;
        g_test.t_last_progress_ms = now;
    } else if (now - g_test.t_last_progress_ms > TEST_STUCK_MS) {
        printf("[audio] test_stream_sine_tick: STUCK phase=%d block=%d off=%d "
               "buffered=%u consumed=%u write_abs=%u — abort\n",
               g_test.phase, g_test.block, g_test.off,
               (unsigned)audio_stream_buffered(),
               (unsigned)dma_consumed_abs(),
               (unsigned)g_write_abs);
        g_test.phase = TEST_PHASE_DONE;
        return AUDIO_TEST_DONE;
    }

    if (g_test.off >= TEST_BLOCK_SAMPLES) {
        g_test.off = 0;
        g_test.block++;
        if (g_test.phase == TEST_PHASE_SINE && g_test.block >= TEST_SINE_BLOCKS) {
            g_test.phase = TEST_PHASE_SILENCE;
            g_test.block = 0;
        } else if (g_test.phase == TEST_PHASE_SILENCE && g_test.block >= TEST_SILENCE_BLOCKS) {
            printf("[audio] test_stream_sine_tick: done in %ums\n",
                   (unsigned)(now - g_test.t_start_ms));
            g_test.phase = TEST_PHASE_DONE;
            return AUDIO_TEST_DONE;
        }
    }
    return AUDIO_TEST_RUNNING;
}

void audio_stop(void) {
    if (!g_audio_ready) return;
    memset(g_audio_buf, 0, sizeof g_audio_buf);
    __sync_synchronize();
    // Snap producer to current DMA consumed count → next write starts
    // empty (used=0, free=max). Underrun snap-forward in write_mono16
    // handles the few-µs gap before the first sample arrives.
    g_write_abs = dma_consumed_abs() & ~1u;
    printf("[audio] STOP\n");
}

// Pause: drop the PIO state machine. DMA loses its DREQ source, so it stops
// requesting words; the ring contents are preserved exactly as-is. Audible
// I2S output goes silent on the next I2S frame boundary (within ~30 µs).
// The upstream pipeline (opus decode + IC ring + g_https_resp + TCP) throttles
// naturally as the audio ring fills past the dispatcher's 75 % threshold.
void audio_pause(void) {
    if (!g_audio_ready || g_audio_paused) return;
    pio_sm_set_enabled(AUDIO_PIO, g_audio_sm, false);
    g_audio_paused = true;
    printf("[audio] PAUSE (buffered=%u/%u frames)\n",
           (unsigned)audio_stream_buffered(), (unsigned)BUF_FRAMES);
}

// Resume: re-enable the PIO state machine. DMA resumes from its preserved
// read position and plays whatever was queued at pause time before the live
// stream catches up. After a long pause the caller will hear up to the
// ring's worth (~170 ms @ 24 kHz) of buffered audio first.
void audio_resume(void) {
    if (!g_audio_ready || !g_audio_paused) return;
    // Flush before re-enabling: anything buffered while paused is stale —
    // the tail of whatever was playing at pause time, plus the end-of-
    // session pad tts_play_pump writes into the frozen ring (observed as
    // RESUME with buffered=4095, then ~256 ms of zombie audio). Zero the
    // whole ring and snap the writer onto the DMA read position so resume
    // is silence until fresh PCM lands. Safe here: resume runs on core0,
    // the same side as every g_write_abs / wrap-tracker user.
    uint32_t stale = (uint32_t)audio_stream_buffered();
    memset(g_audio_buf, 0, sizeof g_audio_buf);
    __sync_synchronize();
    g_write_abs = dma_consumed_abs() & ~1u;
    pio_sm_clkdiv_restart(AUDIO_PIO, g_audio_sm);
    pio_sm_set_enabled(AUDIO_PIO, g_audio_sm, true);
    g_audio_paused = false;
    printf("[audio] RESUME (flushed=%u stale frames)\n", (unsigned)stale);
}

bool audio_is_paused(void) { return g_audio_paused; }

// Underrun handler. If the writer's absolute counter has fallen behind the
// DMA, zero a bounded region ahead of the DMA read position and snap
// g_write_abs forward. Without the zeroing, DMA keeps reading the trailing
// ring_size samples and you hear the last word looping
// ("a-ri-ii-ii-igatou..."). On underrun we'd rather emit silence than
// re-play stale audio.
//
// We DELIBERATELY do not wipe the entire ring: the full wipe creates a
// full ring-duration silence gap on every underrun. Instead we silence a
// fixed ~21ms (@ 24kHz) region starting at the DMA read position — long
// enough to mask the stale-loop while the writer catches up, short enough
// that the audible glitch is brief. If the writer is still behind on the
// next call, this fires again and silences another region (the bound is
// per-call, not cumulative).
//
// Keep this an absolute duration, NOT a fraction of BUF_WORDS — otherwise
// growing the ring proportionally lengthens each silence and eats the
// latency budget the bigger ring was supposed to buy.
//
// `r` is the caller's already-read DMA position. Returns true if an
// underrun was handled (so the caller knows used=0, free=max).
#define UNDERRUN_SILENCE_WORDS  1024u   // ~21ms @ 24kHz stereo (512 frames)
static bool audio_check_underrun(uint32_t r) {
    int32_t diff = (int32_t)(g_write_abs - r);
    if (diff >= 0) return false;

    uint32_t start = r & (BUF_WORDS - 1);
    uint32_t end   = start + UNDERRUN_SILENCE_WORDS;
    if (end <= BUF_WORDS) {
        memset(&g_audio_buf[start], 0, UNDERRUN_SILENCE_WORDS * 4);
    } else {
        uint32_t first = BUF_WORDS - start;
        memset(&g_audio_buf[start], 0, first * 4);
        memset(&g_audio_buf[0],     0, (UNDERRUN_SILENCE_WORDS - first) * 4);
    }
    __sync_synchronize();
    g_write_abs = r & ~1u;   // even-align onto stereo-frame boundary

    static uint32_t last_log = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((now - last_log) >= 250) {
        last_log = now;
        printf("[audio] underrun: silenced %u words (gap=%d words)\n",
               (unsigned)UNDERRUN_SILENCE_WORDS, (int)-diff);
    }
    return true;
}

// SPSC producer: writes mono 16-bit samples into the ring AHEAD of the DMA
// read position. Each input sample becomes TWO ring words (one per channel).
// Absolute counters so we can distinguish "ring full" from "DMA passed the
// writer" (underrun) — the latter triggers audio_check_underrun() which
// silences the ring before snapping forward.
size_t audio_stream_write_mono16(const int16_t *samples, size_t n) {
    if (!g_audio_ready || !samples || n == 0) return 0;

    uint32_t r = dma_consumed_abs();
    (void)audio_check_underrun(r);
    uint32_t w = g_write_abs;
    int32_t  diff = (int32_t)(w - r);
    // diff is guaranteed >= 0 here (audio_check_underrun snapped if it was negative).

    uint32_t used_words = (uint32_t)diff;
    uint32_t free_words = (BUF_WORDS - 1) - used_words;
    uint32_t free_frames = free_words / 2;
    if (free_frames == 0) return 0;
    if (n > free_frames) n = free_frames;

    for (size_t i = 0; i < n; i++) {
        // -6 dB attenuation before I2S to keep the amp out of clipping.
        // /2 is portable on signed int16 (rounds toward zero, sign preserved).
        uint32_t word = pack_i2s_word(samples[i] / 2);   // mono → L=R
        g_audio_buf[w & (BUF_WORDS - 1)] = word;
        w++;
        g_audio_buf[w & (BUF_WORDS - 1)] = word;
        w++;
    }
    __sync_synchronize();
    g_write_abs = w;
    return n;
}

size_t audio_stream_buffered(void) {
    if (!g_audio_ready) return 0;
    // Pure query: refresh the wrap tracker (so dma_consumed_abs doesn't lose
    // a lap between calls) and return the buffered amount. Does NOT run the
    // underrun handler here — that would wipe audio placed by the direct-
    // fill API (audio_play_sine), since those callers don't touch
    // g_write_abs. Streaming clients call audio_stream_underrun_recover()
    // separately when they want the zero-and-snap behavior.
    uint32_t r = dma_consumed_abs();
    int32_t diff = (int32_t)(g_write_abs - r);
    return diff > 0 ? (size_t)(diff / 2) : 0;   // expose as STEREO FRAMES
}

// Streaming-only: if the writer has been lapped by DMA, zero the ring and
// snap g_write_abs forward. Without this, a starved producer would hear
// the most-recent ring_size samples loop ("a-ri-i-i-igatou..."). Safe to
// call every iteration; no-op when there's no underrun. NOT called from
// audio_stream_buffered() because the direct-fill APIs (audio_play_sine)
// intentionally keep g_write_abs behind r.
bool audio_stream_underrun_recover(void) {
    if (!g_audio_ready) return false;
    return audio_check_underrun(dma_consumed_abs());
}

void audio_stream_reset(void) {
    if (!g_audio_ready) return;
    memset(g_audio_buf, 0, sizeof g_audio_buf);
    __sync_synchronize();
    // Snap producer's absolute counter to DMA's current absolute consumed
    // count → next write sees used=0, free=max. Even-align so we begin on
    // a stereo-frame boundary (R-slot).
    g_write_abs = dma_consumed_abs() & ~1u;
}
