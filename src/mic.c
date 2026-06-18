#include "mic.h"
#include "common.h"
#include "ic_ring.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "i2s_in.pio.h"
#include "opus.h"
#include "opus_scratch.h"
#include "opus_stream.h"

#include <stdio.h>
#include <string.h>

//=============================================================================
// Wiring (Waveshare PicoLCD-1.3 user-button pins, hijacked while recording):
//   BCK  GP16   (also Key-Left  — non-functional during capture)
//   LRCK GP20   (also Key-Right — non-functional during capture)
//   SD   GP21   (also Button-Y / PROV_BUTTON_PIN — read at boot before init,
//                fine; non-functional once capture starts)
// mic_stop() restores them to GPIO input + pull-up so the buttons work again.
//=============================================================================
#define MIC_PIO          pio0       // pio2 is owned by CYW43+audio; use pio0
#define MIC_BCK_PIN      16
#define MIC_LRCK_PIN     20
#define MIC_SD_PIN       21

//=============================================================================
// Opus encoder config — voice quality vs PTT length trade-off
//   - VOIP application, SILK-only at 16 kHz
//   - 16 kbps CBR → ~40 bytes per 20 ms frame (8 kbps sounded audibly
//     gritty/hissy on echo playback; 16 kbps is SILK's comfortable floor
//     for WB voice). NOTE: halves the max PTT take vs 8 kbps —
//     LP_OPUS_CAP (12 KB) now fills in ~5.6 s; [lp/accum] logs drops.
//   - complexity 0 → cheapest CPU (do NOT raise: core0 stack hiwater is
//     18,912/20,480 B during encode — only ~1.5 KB headroom)
//=============================================================================
#define MIC_OPUS_BITRATE     16000
#define MIC_OPUS_COMPLEXITY  0
#define MIC_OPUS_MAX_PKT     128    // generous upper bound for 16 kbps × 20 ms

//=============================================================================
// Digital mic gain. The INMP441 is rated -26 dBFS @ 94 dB SPL, so normal
// speech at conversational distance lands around -50..-35 dBFS — a few
// hundred counts after the >>16 extraction (measured: mean|s|=436,
// [mic/dump] showed ±150 mid-take while speaking). That encodes to a
// near-inaudible whisper. Shift the full 24-bit sample up ×2^MIC_GAIN_SHIFT
// with saturation; clips are counted and reported in the STOP summary.
// ×8 hard-clipped audibly on plosives/breath pops at handset distance
// (raw peaks ±12-21 K × 8 ≫ 32767), so: ×4 gain + a 4:1 soft knee above
// ±24576 instead of hard saturation — overshoot up to ~57 K compresses
// smoothly, only beyond that hard-caps (counted as clips). Tune with the
// STOP stats: aim mean|s| ≈ 1500-6000, clips ≈ 0.
//=============================================================================
#define MIC_GAIN_SHIFT   2      // ×4 vs plain >>16 extraction
#define MIC_KNEE         24576  // soft-knee threshold (post-gain)
#define MIC_KNEE_HEADROOM 8191  // knee output range above threshold (→32767)

//=============================================================================
// PIO RX ring (DMA writes 32-bit words here, two per stereo frame).
// 1024 words = 4 KB; ~32 ms cushion @ 16 kHz stereo capture, easily covers
// the main loop's 1 ms tick.
//=============================================================================
// 2048 words = 8 KB; ~64 ms cushion @ 16 kHz stereo capture. Was 1024/32 ms,
// but core0's loop provably stalls past 32 ms at times (measured 25% sample
// loss on a 2.5 s take: pump gaps lap the ring AND alias the modulo delta in
// mic_dma_produced_abs(), so the loss was silent). The pump-gap stats printed
// at STOP verify whether 64 ms is enough.
#define MIC_RING_WORDS   2048
#define MIC_RING_BYTES   (MIC_RING_WORDS * 4)
#define MIC_RING_BITS    13         // log2(MIC_RING_BYTES)

static uint32_t __attribute__((aligned(MIC_RING_BYTES))) g_mic_ring[MIC_RING_WORDS];

static int          g_mic_sm        = -1;
static int          g_mic_dma_ch    = -1;
static uint         g_mic_pio_off   = 0;
static OpusEncoder *g_mic_enc       = NULL;
static bool         g_mic_inited    = false;
static volatile bool g_mic_active   = false;

// PCM accumulator: PIO produces stereo words; we collect 16-bit mono samples
// (left channel only) into pcm_acc[] until we have a full 20 ms Opus frame.
static int16_t  g_mic_pcm_acc[MIC_FRAME_SAMPLES];
static size_t   g_mic_pcm_acc_len   = 0;

// DMA write-position tracking. Same trick as audio.c: derive an absolute word
// counter from the modulo write_addr + a software wrap count, so we can tell
// "ring empty" from "DMA lapped the reader".
static uint32_t g_mic_read_abs      = 0;
static uint32_t g_mic_last_ring_pos = 0;
static uint32_t g_mic_produced_abs  = 0;

// Pump-cadence diagnostics: the modulo-delta in mic_dma_produced_abs()
// silently aliases when a pump gap exceeds one ring period (samples vanish
// uncounted — measured 25% loss with the old 32 ms ring), so track the gaps
// and report worst-case + overrun count at STOP.
static uint32_t g_mic_start_ms      = 0;
static uint32_t g_mic_pump_last_ms  = 0;
static uint32_t g_mic_pump_max_gap  = 0;
static uint32_t g_mic_overruns      = 0;

// Per-recording PCM-level diagnostics. CBR Opus hides silence-vs-speech, so
// inspect the raw samples directly to tell a live mic from a dead/stuck line:
//   peak ~0                  → no signal (mic unpowered / SD not wired)
//   and==or (single value)   → line stuck at a constant (e.g. floating high)
//   wide min..max + variance  → real audio
static int32_t  g_mic_dbg_min   = 0;
static int32_t  g_mic_dbg_max   = 0;
static uint32_t g_mic_dbg_or    = 0;       // OR of raw 16-bit samples
static uint32_t g_mic_dbg_and   = 0xFFFF;  // AND of raw 16-bit samples
static uint32_t g_mic_dbg_n     = 0;       // samples seen
static uint64_t g_mic_dbg_absum = 0;       // sum of |sample| → mean level

// One-shot per-take waveform dumps, chasing the "STOP stats look alive but
// the encoded packets decode to silence" failure: the raw ring words show
// which channel slot carries data and where the 24-bit field sits (a
// tri-stated slot dragged around by the GP21 pull-up can fake signal-like
// min/max stats); the extracted PCM shows the waveform the encoder actually
// sees (voice = smooth/correlated; bit-misalignment = broadband garbage;
// subsonic or Nyquist-heavy artifacts = silence after SILK's filters).
static bool g_mic_dump_raw_done;
static bool g_mic_dump_pcm_done;
static uint32_t g_mic_dbg_clips = 0;       // samples saturated by MIC_GAIN_SHIFT

bool mic_is_active(void) { return g_mic_active; }

//=============================================================================
// Ring helpers
//=============================================================================
static inline uint32_t mic_dma_write_ring_pos(void) {
    uintptr_t base = (uintptr_t)g_mic_ring;
    uintptr_t wa   = (uintptr_t)dma_hw->ch[g_mic_dma_ch].write_addr;
    uint32_t off_words = (uint32_t)((wa - base) >> 2);
    return off_words & (MIC_RING_WORDS - 1);
}

static inline uint32_t mic_dma_produced_abs(void) {
    uint32_t now = mic_dma_write_ring_pos();
    uint32_t delta = (now - g_mic_last_ring_pos) & (MIC_RING_WORDS - 1);
    g_mic_produced_abs += delta;
    g_mic_last_ring_pos = now;
    return g_mic_produced_abs;
}

//=============================================================================
// Init
//=============================================================================

// Create + configure the opus encoder. Deferred out of mic_init() and into
// mic_start() so the ~29 KB encoder state only occupies the newlib heap while
// the mic is actually recording. During playback that same RAM is needed by
// the opus decoder (~18 KB) plus the mbedtls TLS context — on this RP2350 the
// heap (~56 KB) cannot hold encoder + decoder + TLS at once, which is why a
// boot-time encoder made the first TTS playback OOM.
static int mic_encoder_open(void) {
    if (g_mic_enc) return 0;
    int err = OPUS_OK;
    g_mic_enc = opus_encoder_create(MIC_SAMPLE_RATE_HZ, 1,
                                    OPUS_APPLICATION_VOIP, &err);
    if (!g_mic_enc || err != OPUS_OK) {
        printf("[mic] opus_encoder_create failed err=%d\n", err);
        g_mic_enc = NULL;
        return -1;
    }
    opus_encoder_ctl(g_mic_enc, OPUS_SET_BITRATE(MIC_OPUS_BITRATE));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_VBR(0));                    // CBR
    opus_encoder_ctl(g_mic_enc, OPUS_SET_COMPLEXITY(MIC_OPUS_COMPLEXITY));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(g_mic_enc, OPUS_SET_FORCE_CHANNELS(1));
    // Tried NARROWBAND here hoping to shrink SILK's VAR_ARRAYS stack VLAs:
    // no effect (encode peak 18,744 B WB cap vs 18,912 B NB cap, stackdiag
    // measured) — at 8 kbps CBR the encoder already picks NB internally,
    // so neither cap binds.
    opus_encoder_ctl(g_mic_enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
    return 0;
}

static void mic_encoder_close(void) {
    if (g_mic_enc) {
        opus_encoder_destroy(g_mic_enc);
        g_mic_enc = NULL;
    }
}

int mic_init(void) {
    if (g_mic_inited) return 0;
    printf("[mic] init begin\n");

    memset(g_mic_ring, 0, sizeof g_mic_ring);

    g_mic_sm      = pio_claim_unused_sm(MIC_PIO, true);
    g_mic_pio_off = pio_add_program(MIC_PIO, &i2s_in_program);
    g_mic_dma_ch  = dma_claim_unused_channel(true);
    g_mic_inited  = true;

    // The encoder is opened on demand in mic_start() (push-to-talk), not here.
    printf("[mic] ready: pio=%p sm=%d dma_ch=%d (16k mono VOIP %d bps) "
           "enc deferred (get_size=%d B, opened on PTT)\n",
           (void*)MIC_PIO, g_mic_sm, g_mic_dma_ch,
           MIC_OPUS_BITRATE, opus_encoder_get_size(1));
    return 0;
}

//=============================================================================
// Start / stop
//=============================================================================
// PIO cycles per stereo frame: two channels × (4 setup + 64 loop) = 136.
// Must match the i2s_in.pio program layout exactly or the WS rate drifts off
// 16 kHz.
#define MIC_FRAME_CYCLES  136.0f

static void mic_apply_clkdiv(void) {
    // Frame = 136 PIO cycles → PIO clock = 16000 × 136 = 2.176 MHz @ 16 kHz.
    float sys_hz = (float)clock_get_hz(clk_sys);
    float div    = sys_hz / ((float)MIC_SAMPLE_RATE_HZ * MIC_FRAME_CYCLES);
    pio_sm_set_clkdiv(MIC_PIO, g_mic_sm, div);
    pio_sm_clkdiv_restart(MIC_PIO, g_mic_sm);
    printf("[mic] clk_sys=%.0f div=%.3f → PIO=%.0fHz BCK~%.0fHz frame=%.0fHz\n",
           (double)sys_hz, (double)div, (double)(sys_hz / div),
           (double)(sys_hz / div / 2.0f),
           (double)(sys_hz / div / MIC_FRAME_CYCLES));
}

void mic_start(void) {
    if (!g_mic_inited) {
        printf("[mic] start: not initialized\n");
        return;
    }
    if (g_mic_active) return;

    // Half-duplex push-to-talk: gui.c has already audio_pause()'d the receive
    // path, so the opus decoder is idle. Free it to return ~18 KB to the heap
    // before allocating the ~29 KB encoder — they never run at once, and the
    // heap can't hold both plus the TLS context. opus_stream re-creates the
    // decoder lazily on the next decode after audio_resume().
    opus_stream_free();
    if (mic_encoder_open() != 0) {
        printf("[mic] start: encoder open failed\n");
        return;
    }

    // Fresh encoder is already reset; just clear the PCM accumulator.
    g_mic_pcm_acc_len   = 0;

    // Reset PCM diagnostics for this recording.
    g_mic_dbg_min   = 32767;
    g_mic_dbg_max   = -32768;
    g_mic_dbg_or    = 0;
    g_mic_dbg_and   = 0xFFFF;
    g_mic_dbg_n     = 0;
    g_mic_dbg_absum = 0;
    g_mic_dump_raw_done = false;
    g_mic_dump_pcm_done = false;
    g_mic_dbg_clips     = 0;

    // Configure pins as PIO function. i2s_in_program_init also sets pindirs.
    i2s_in_program_init(MIC_PIO, g_mic_sm, g_mic_pio_off,
                        MIC_BCK_PIN, MIC_LRCK_PIN, MIC_SD_PIN);
    mic_apply_clkdiv();

    // Wipe ring and reset progress trackers BEFORE arming DMA so the first
    // mic_dma_write_ring_pos() reads a known baseline.
    memset(g_mic_ring, 0, sizeof g_mic_ring);
    g_mic_read_abs      = 0;
    g_mic_last_ring_pos = 0;
    g_mic_produced_abs  = 0;
    g_mic_start_ms      = board_millis();
    g_mic_pump_last_ms  = g_mic_start_ms;
    g_mic_pump_max_gap  = 0;
    g_mic_overruns      = 0;

    // DMA: read PIO RX FIFO into ring, ENDLESS write-side ring wrap.
    dma_channel_config cfg = dma_channel_get_default_config(g_mic_dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(MIC_PIO, g_mic_sm, false));
    channel_config_set_ring(&cfg, /*write=*/true, MIC_RING_BITS);
    dma_channel_configure(
        g_mic_dma_ch, &cfg,
        g_mic_ring,
        &MIC_PIO->rxf[g_mic_sm],
        dma_encode_endless_transfer_count(),
        /*start=*/true);

    pio_sm_clear_fifos(MIC_PIO, g_mic_sm);
    pio_sm_restart(MIC_PIO, g_mic_sm);
    pio_sm_set_enabled(MIC_PIO, g_mic_sm, true);

    mem_barrier();
    g_mic_active = true;
    printf("[mic] START\n");
}

void mic_stop(void) {
    if (!g_mic_active) return;
    g_mic_active = false;
    mem_barrier();

    pio_sm_set_enabled(MIC_PIO, g_mic_sm, false);
    dma_channel_abort(g_mic_dma_ch);

    // Restore GP16/20/21 to plain GPIO + pull-up so the Waveshare buttons
    // (Key-Left, Key-Right, Button-Y) work again.
    const uint8_t pins[] = { MIC_BCK_PIN, MIC_LRCK_PIN, MIC_SD_PIN };
    for (size_t i = 0; i < sizeof pins; i++) {
        gpio_set_function(pins[i], GPIO_FUNC_SIO);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
    g_mic_pcm_acc_len = 0;

    // Report the captured PCM level so we can tell a live mic from a dead line.
    uint32_t mean = g_mic_dbg_n ? (uint32_t)(g_mic_dbg_absum / g_mic_dbg_n) : 0;
    printf("[mic] STOP — pcm: n=%lu min=%ld max=%ld mean|s|=%lu clips=%lu "
           "or=%04lx and=%04lx%s\n",
           (unsigned long)g_mic_dbg_n,
           (long)g_mic_dbg_min, (long)g_mic_dbg_max,
           (unsigned long)mean,
           (unsigned long)g_mic_dbg_clips,
           (unsigned long)(g_mic_dbg_or  & 0xFFFF),
           (unsigned long)(g_mic_dbg_and & 0xFFFF),
           (g_mic_dbg_max - g_mic_dbg_min < 64) ? "  <-- FLAT/NO SIGNAL" : "");

    // Capture-integrity summary: expected samples from wall clock vs actually
    // pumped. loss > ~2% or overruns > 0 → the ring cushion was breached
    // (robo-voice); check max_gap to see how long core0 stalled.
    uint32_t dur_ms = board_millis() - g_mic_start_ms;
    uint32_t expect = dur_ms * (MIC_SAMPLE_RATE_HZ / 1000u);
    uint32_t loss   = (expect > g_mic_dbg_n)
                      ? (uint32_t)((uint64_t)(expect - g_mic_dbg_n) * 100u / expect)
                      : 0;
    printf("[mic] capture: dur=%lums expect~%lu got=%lu loss=%lu%% "
           "max_pump_gap=%lums overruns=%lu (ring=%ums)\n",
           (unsigned long)dur_ms, (unsigned long)expect,
           (unsigned long)g_mic_dbg_n, (unsigned long)loss,
           (unsigned long)g_mic_pump_max_gap, (unsigned long)g_mic_overruns,
           (unsigned)(MIC_RING_WORDS / 32u));

    // Release the ~29 KB encoder so the heap is free for the decoder + TLS
    // when audio_resume() brings the receive path back. Safe here: g_mic_active
    // is already false and the pump runs on this same core, so no frame can be
    // encoding. opened again on the next mic_start().
    mic_encoder_close();
}

//=============================================================================
// Pump: drain DMA ring, encode 20 ms frames, ship via IC ring
//=============================================================================
static void mic_ship_frame(void) {
    uint8_t pkt[MIC_OPUS_MAX_PKT];
    opus_int32 n = opus_encode(g_mic_enc, g_mic_pcm_acc,
                                MIC_FRAME_SAMPLES, pkt, sizeof pkt);
    if (!opus_scratch_canary_ok()) {
        // opus overran its BSS pseudostack (see src/opus_scratch.c) and has
        // corrupted adjacent BSS — bail loudly rather than ship garbage.
        panic("[mic] opus pseudostack overflow");
    }
    if (n <= 0) {
        printf("[mic] opus_encode err=%ld (frame skipped)\n", (long)n);
        return;
    }
    // ic_send_avail covers both ring space AND notification FIFO; if either
    // is full, drop this frame rather than stall core0 — losing 20 ms of mic
    // audio is better than wedging the IC bus.
    uint32_t need = 4u + ((uint32_t)((n + 3) & ~3));
    if (ic_send_avail() < need) {
        static uint32_t last_drop_log = 0;
        uint32_t now = board_millis();
        if ((now - last_drop_log) >= 500) {
            last_drop_log = now;
            printf("[mic] ship-block: need=%u avail=%u — frame dropped\n",
                   (unsigned)need, (unsigned)ic_send_avail());
        }
        return;
    }
    ic_send(IC_MSG_LINEPHONE_OPUS_PKT, pkt, (uint16_t)n);
}

void mic_pump(void) {
    if (!g_mic_active) return;

    // Stall watch: DMA produces 32 words/ms, so a pump gap of one ring
    // period (MIC_RING_WORDS/32 ms) or more means the writer lapped the
    // reader and mic_dma_produced_abs()'s modulo delta aliased — samples
    // are gone and uncounted. Can't recover them; record the event so the
    // STOP summary shows whether the ring cushion is still being breached.
    uint32_t now_ms = board_millis();
    uint32_t gap    = now_ms - g_mic_pump_last_ms;
    g_mic_pump_last_ms = now_ms;
    if (gap > g_mic_pump_max_gap) g_mic_pump_max_gap = gap;
    if (gap >= MIC_RING_WORDS / 32u) g_mic_overruns++;

    uint32_t produced = mic_dma_produced_abs();

    // Raw slot dump, once per take, ~32 ms in (skip the spin-up frames).
    // One UART line ≈ 160 B ≈ 14 ms @115200 — inside the 64 ms ring cushion.
    if (!g_mic_dump_raw_done && produced >= 1024u
        && g_mic_read_abs + 16u <= produced) {
        g_mic_dump_raw_done = true;
        printf("[mic/dump] raw (L,R)x8 @abs=%lu:",
               (unsigned long)g_mic_read_abs);
        for (int i = 0; i < 16; i += 2) {
            printf(" %08lx,%08lx",
                   (unsigned long)g_mic_ring[(g_mic_read_abs + (uint32_t)i)
                                             & (MIC_RING_WORDS - 1)],
                   (unsigned long)g_mic_ring[(g_mic_read_abs + (uint32_t)i + 1)
                                             & (MIC_RING_WORDS - 1)]);
        }
        printf("\n");
    }

    while (g_mic_read_abs + 2u <= produced) {
        // Consume one stereo frame (2 words: left, right). PIO autopush is
        // MSB-first (shift_left), so each word holds a left-justified 16-bit
        // sample in bits [31:16] — extract that as the signed PCM.
        uint32_t left_word = g_mic_ring[g_mic_read_abs       & (MIC_RING_WORDS - 1)];
        // right_word is discarded (L/R tied to GND → right slot is zero).
        (void)            g_mic_ring[(g_mic_read_abs + 1)    & (MIC_RING_WORDS - 1)];
        g_mic_read_abs += 2;

        // Full 24-bit sample lives in bits [31:8]; arithmetic >>8 sign-
        // extends it. Apply digital gain (see MIC_GAIN_SHIFT) — plain >>16
        // truncation leaves conversational speech ~40 dB below full scale.
        int32_t s24 = (int32_t)left_word >> 8;
        int32_t amp = s24 >> (8 - MIC_GAIN_SHIFT);
        // Soft knee: linear to ±MIC_KNEE, then 4:1 compression of the
        // overshoot into the remaining headroom; hard cap (counted) only
        // past ~±57 K. Keeps plosives from turning into square waves.
        int32_t mag = amp < 0 ? -amp : amp;
        if (mag > MIC_KNEE) {
            int32_t over = (mag - MIC_KNEE) >> 2;
            if (over > MIC_KNEE_HEADROOM) {
                over = MIC_KNEE_HEADROOM;
                g_mic_dbg_clips++;
            }
            mag = MIC_KNEE + over;
            amp = (amp < 0) ? -mag : mag;
        }
        int16_t s = (int16_t)amp;

        // PCM-level diagnostics (see g_mic_dbg_* declarations).
        uint32_t raw = (uint32_t)(uint16_t)s;
        g_mic_dbg_or  |= raw;
        g_mic_dbg_and &= raw;
        if (s < g_mic_dbg_min) g_mic_dbg_min = s;
        if (s > g_mic_dbg_max) g_mic_dbg_max = s;
        g_mic_dbg_absum += (uint32_t)(s < 0 ? -s : s);
        g_mic_dbg_n++;

        g_mic_pcm_acc[g_mic_pcm_acc_len++] = s;
        if (g_mic_pcm_acc_len == MIC_FRAME_SAMPLES) {
            mic_ship_frame();
            g_mic_pcm_acc_len = 0;
        }
    }

    // PCM waveform dump, once per take, ~1 s in (well past spin-up, while
    // the user is presumably speaking). 32 consecutive samples = 2 ms.
    if (!g_mic_dump_pcm_done && g_mic_dbg_n >= 16000u
        && g_mic_pcm_acc_len >= 32u) {
        g_mic_dump_pcm_done = true;
        printf("[mic/dump] pcm x32 @n=%lu:", (unsigned long)g_mic_dbg_n);
        for (size_t i = g_mic_pcm_acc_len - 32u; i < g_mic_pcm_acc_len; i++) {
            printf(" %d", (int)g_mic_pcm_acc[i]);
        }
        printf("\n");
    }
}
