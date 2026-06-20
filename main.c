//=============================================================================
// main.c — Inter-core state machine and event dispatch.
//
// Core 1: CYW43 WiFi + HTTPS + TTS forwarding
// Core 0: LCD + buttons + audio ring drain
//=============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>   // sbrk(0) — newlib heap break probe (stackdiag)
#include <math.h>     // fabsf — sonar change-threshold logging

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"   // vreg_set_voltage for 200 MHz overclock
#include "hardware/sync.h"           // save_and_disable_interrupts (BOOTSEL read)
#include "hardware/structs/ioqspi.h" // QSPI SS pad override (BOOTSEL read)
#include "hardware/structs/sio.h"    // gpio_hi_in (BOOTSEL read)

#include "common.h"
#include "st7789.h"
#include "audio.h"
#include "ic_ring.h"
#include "http.h"
#include "queue.h"
#include "gui.h"
#include "led.h"
#include "wifi.h"
#include "tts.h"
#include "opus_stream.h"
#include "credentials.h"
#include "ble_provision.h"
#include "mic.h"
#include "linephone.h"
#include "hc_sr04.h"

//=============================================================================
// Configuration — only API_URL stays at build time; WiFi creds *and*
// per-device DEVICE_ID are provisioned over BLE and loaded from flash.
//=============================================================================
#ifndef API_URL
#define API_URL         "https://d2bbeowpg2f545.cloudfront.net/"
#endif

// Two boot-time triggers force BLE provisioning mode:
//  - GP21 held LOW (Button-Y on PicoLCD-1.3) — needs the display attached.
//  - The Pico 2 W onboard BOOTSEL button — for headless/display-less boards
//    where GP21 isn't wired to anything pressable. Read at runtime via the
//    QSPI-CS trick (see read_bootsel_button below).
#define PROV_BUTTON_PIN  21

#define TIMELINE_POLL_INTERVAL_MS 10000

// HC-SR04 ultrasonic range finder. TRIG=GP6, ECHO=GP7 (after the 5V→3.3V
// divider). GP18/GP19 were NOT usable: they are the Waveshare board's Key-Down
// and Button-X (src/gui.c g_buttons[]), wired as polled pull-up inputs — the
// pull-up held ECHO high and the trigger pulses faked button presses. GP6/GP7
// are free (UART=0/1, joy/btn=2/3/15/17/18/19, LCD=8-13, mic=16/20/21,
// audio-out=26-28). Driven entirely by PIO (src/hc_sr04.pio) on pio1 — pio0
// holds the mic, pio2 holds CYW43 PIO-SPI + audio out. One ping every 100 ms.
#define SONAR_PIO          pio1
#define SONAR_TRIG_PIN     6
#define SONAR_ECHO_PIN     7
#define SONAR_PING_INTERVAL_MS 100
#define SONAR_LOG_DELTA_CM     2.0f   // only log a valid reading when it moves this much
// Capture LED: lit while the sonar push-to-talk mic is ACTIVE (object within
// SONAR_NEAR_CM), and dark once the 3 s auto-POST forces the mic OFF — it does
// NOT track raw proximity, so a lingering object won't keep it lit. Driven by
// the mic SM near the bottom of the loop, not the ping block.
// Plain GPIO (active-high), NOT the CYW43 onboard LED — wire GP5 → ~330Ω →
// LED(+) → LED(−) → GND. GP5 is free (sonar uses GP6/GP7; see pin map above).
#define SONAR_LED_PIN          5
#define SONAR_NEAR_CM          10.0f
// Sonar proximity push-to-talk: while an object is within SONAR_NEAR_CM the
// mic runs exactly like a held Button-B (gui_button_b). Capped at
// SONAR_MIC_MAX_MS (forced ON→OFF even if still near), then a
// SONAR_MIC_COOLDOWN_MS lockout before it can re-trigger.
#define SONAR_MIC_MAX_MS       3000
#define SONAR_MIC_COOLDOWN_MS  1000
// Only POST the capture if the mic stayed on at least this long; shorter
// blips are discarded (no upload) so a quick swipe past the sensor is ignored.
#define SONAR_MIC_MIN_POST_MS  1000

//=============================================================================
// Runtime WiFi credentials — loaded from flash by core0 main(), consumed by
// core1's network_init() once both cores are up.
//=============================================================================
static wifi_creds_t g_wifi_creds;

// HC-SR04 range finder handle (PIO1). Initialized in on_core0_audio_beep_play,
// pinged/read each main-loop pass in on_core0_running.
static hc_sr04_t g_sonar;

//=============================================================================
// opus memory layout on RP2350 (settled design; git 2936d8b has the journey —
// it prevents an encode stack OVERFLOW, a boot OOM, and a playback OOM).
//
//   high | __StackTop
//        | core0 stack       encode hiwater ~3.6 KB  (was >=23.5 KB as VLAs)
//   ~~~~ + ~~~~~~~~ __StackBottom == __HeapLimit
//        | newlib heap ~56 KB   shared: cyw43 / lwIP / mbedtls + opus codec
//        + ----------------
//        | BSS
//        | g_opus_scratch 24 KB   <- opus SILK/CELT scratch lives HERE,
//   low  |                           off the stack
//
// Invariants — break any one and it regresses to a crash:
//   1. opus is built NONTHREADSAFE_PSEUDOSTACK, not VAR_ARRAYS — scratch in
//      the BSS buffer above, not VLAs on this stack. (cmake.rp2350)
//   2. GLOBAL_STACK_SIZE (cmake -D) == OPUS_SCRATCH_BYTES (opus_scratch.h),
//      both 24576. Mismatch -> opus writes past the canary -> BSS corruption.
//   3. mic encoder (~29 KB) and stream decoder (~18 KB) never coexist on the
//      heap: half-duplex PTT frees one before opening the other (mic.c /
//      opus_stream.c). Peak heap = 38 KB playback, 49 KB record, both < 56 KB.
//=============================================================================

//=============================================================================
// STACKDIAG — measurement-only instrumentation (no behavior change).
//
// Goal: pick a core0 stack size that fits the opus ENCODER (mic) without
// starving the shared newlib heap that cyw43/lwIP/mbedtls allocate from.
// __HeapLimit == __StackBottom, so stack and heap fight over the same RAM;
// growing PICO_STACK_SIZE shrinks the heap 1:1 (that's why 0x6000 broke WiFi).
// These three probes give the real numbers so we stop guessing:
//   (A) opus_{encoder,decoder}_get_size — printed in mic.c / opus_stream.c
//   (B) free-heap probe after network is up   — stackdiag_heap_report()
//   (C) core0 stack high-water via canary paint — stackdiag_paint/hiwater()
//=============================================================================
#define STACK_CANARY 0xC5C5C5C5u
// How far BELOW __StackBottom the canary paint extends. Historically the
// stack region pegged at hiwater=16384/16384 during mic encode (opus was
// then built with VAR_ARRAYS — all SILK scratch was VLAs on the caller's
// stack; now NONTHREADSAFE_PSEUDOSTACK, see banner above, so encode peaks
// at ~3.6 KB), so the real peak was somewhere past the bottom. The area
// below is the newlib heap's growth gap (break was 84 KB below
// __StackBottom at net-ready);
// painting its top 16 KB is safe as long as the break never climbs that
// high — stackdiag checks sbrk(0) at paint and scan time and degrades to
// the in-region measurement if it does. This measures the overflow depth
// WITHOUT raising PICO_STACK_SIZE (which has history: 0x6000 broke WiFi).
#define STACKDIAG_GAP_PAINT (16u * 1024u)
// Linker-provided region bounds (see memmap_bigstack.ld).
extern char __StackBottom[];
extern char __StackTop[];
extern char __HeapLimit[];

// Lowest address the canary paint/scan may touch: 16 KB below the stack
// region, clamped to the live heap break (+1 KB guard) if it's already up
// there. Recomputed at scan time so heap growth can only shrink the probed
// window, never let the scan read heap-owned bytes as "stack usage".
static uintptr_t stackdiag_floor(void) {
    uintptr_t floor = (uintptr_t)__StackBottom - STACKDIAG_GAP_PAINT;
    uintptr_t brk_guard = ((uintptr_t)sbrk(0) + 1024u + 3u) & ~3u;
    return (brk_guard > floor) ? brk_guard : floor;
}

// Paint the unused part of core0's stack with a known pattern so we can later
// find how deep it was ever used. Call ONCE, early in main(), while the stack
// is still shallow (SP near __StackTop) so almost the whole region is painted.
// Leaves a 256 B guard below the live SP so we never scribble the live frame.
static void stackdiag_paint(void) {
    uintptr_t sp;
    __asm volatile ("mov %0, sp" : "=r"(sp));
    uint32_t *p   = (uint32_t *)stackdiag_floor();
    uint32_t *end = (uint32_t *)((sp - 256) & ~3u);
    while (p < end) *p++ = STACK_CANARY;
}

// Scan from the floor up; the first word that is no longer the canary marks
// the deepest the stack has ever reached. Returns peak bytes used (depth from
// __StackTop) — values ABOVE the region size mean the stack overflowed into
// the gap below __StackBottom by that excess. Monotonic across the run —
// captures encode AND decode peaks.
static size_t stackdiag_hiwater(void) {
    const uint32_t *p   = (const uint32_t *)stackdiag_floor();
    const uint32_t *top = (const uint32_t *)(uintptr_t)__StackTop;
    while (p < top && *p == STACK_CANARY) p++;
    return (size_t)((const char *)top - (const char *)p);
}

// Report newlib-heap availability at a given moment. growable = how far the
// break can still rise toward __HeapLimit — an UPPER BOUND on what a single
// malloc (e.g. altcp_tls_new's ~16 KB record buffer) can still get; free-list
// holes below the break may allow a bit more. sbrk(0) only reads the current
// break, so this never allocates — important because pico_malloc's wrapper
// (PICO_MALLOC_PANIC, default ON) panics on a failed malloc, so probing the
// limit with real allocations would itself be the OOM crash.
// Upper bound on what a single fresh malloc can still get: how far the break
// can rise toward __HeapLimit. Same figure stackdiag_heap_report() prints; the
// linephone poll uses it to refuse a TLS handshake it can't afford (see
// LP_POLL_MIN_GROWABLE_B). Free-list holes below the break may allow a bit
// more, so this errs on the safe (skip) side.
static size_t heap_growable(void) {
    return (size_t)((uintptr_t)__HeapLimit - (uintptr_t)sbrk(0));
}

static void stackdiag_heap_report(const char *when) {
    uintptr_t brk      = (uintptr_t)sbrk(0);
    size_t    growable = (size_t)((uintptr_t)__HeapLimit - brk);
    printf("[stackdiag] heap @%s: break=0x%08x limit=0x%08x growable=%u B\n",
           when, (unsigned)brk, (unsigned)(uintptr_t)__HeapLimit,
           (unsigned)growable);
}

//=============================================================================
// Inter-core state machine — types and shared state
//=============================================================================
typedef enum {
    CORE0_S_INIT = 0,
    CORE0_S_WAIT_NET,
    CORE0_S_AUDIO_BEEP_PLAY,
    CORE0_S_AUDIO_BEEP_WAIT,
    CORE0_S_AUDIO_TEST_START,
    CORE0_S_AUDIO_TEST_TICK,
    CORE0_S_AUDIO_READY,
    CORE0_S_RUNNING,
} core0_state_t;

typedef enum {
    CORE1_S_INIT = 0,
    CORE1_S_NETWORK,
    CORE1_S_WAIT_AUDIO,
    CORE1_S_RUNNING,
} core1_state_t;

static volatile core0_state_t g_core0_state = CORE0_S_INIT;
static volatile core1_state_t g_core1_state = CORE1_S_INIT;

// Sized to fit the largest single IC message: opus packets shipped from
// core1 via IC_MSG_TTS_OPUS_PKT. RFC 6716 allows up to 1275 B per packet
// (OPUS_STREAM_MAX_PACKET = 1500 is the defensive upper bound); 1536 here
// rounds up with headroom so the recv buffer never truncates a packet.
#define IC_RECV_BUF_BYTES 1536
static uint8_t g_core0_recv_buf[IC_RECV_BUF_BYTES];
static uint8_t g_core1_recv_buf[IC_RECV_BUF_BYTES];

//=============================================================================
// Core 1 — private state
//=============================================================================
static bool     g_core1_fetch_pending = false;
static uint32_t g_timeline_last_poll  = 0;
static bool     g_core1_tl_active     = false;
// Operator id the device is currently "talking to" — captured from the
// IC payload of whichever start signal core0 last sent (TIMELINE_START
// or RADIO_START; the two modes are mutually exclusive). Pinned into
// outgoing TTS/radio request bodies so the server's default routing
// can't divert the response to another operator registered for this
// device. Empty string means "no target chosen yet".
static char     g_core1_current_op_id[MAX_LAB_ID_LEN] = "";
static bool     g_core1_radio_pending = false;
// Linephone (walkie-talkie) capture-then-post state. While core0 is shipping
// opus packets we just accumulate them; on POST_END we set the pending flag
// so the main loop kicks the HTTPS request when no other transport is busy.
static bool     g_core1_lp_post_pending = false;
// Active linephone session — set by IC_MSG_LINEPHONE_START / cleared by
// IC_MSG_LINEPHONE_STOP. Gates the GET polling loop.
static bool     g_core1_lp_session_active = false;
// GET poll cadence (fires only while session is active and nothing else is
// in flight). 404 responses are tiny (just headers) but each request costs a
// fresh TLS handshake (~100 ms CPU + a few KB of traffic), so this is the
// main lever for trading off latency vs. load. 10 s gives a relaxed debug-
// friendly cadence; drop to 2–3 s for a more "real" walkie-talkie feel.
// NOTE: short intervals keep the device almost always mid-playback, so
// pressing B to record collides with in-flight RX (audio PAUSE/RESUME +
// throttled drain). 10 s leaves silent gaps so recording lands in silence.
#define LP_POLL_INTERVAL_MS  10000
// Heap headroom a poll must see before it dares open a TLS connection. The
// handshake's altcp_tls_new record buffer alone is ~16 KB (see main.c heap
// notes); 24 KB leaves margin for the transient lwIP pbufs + request buffer.
// If growable is below this — e.g. the previous TTS decoder (~18 KB) is still
// resident and fragmentation has eaten the rest — we SKIP the poll this cycle
// rather than let pico_malloc's PICO_MALLOC_PANIC turn a failed 16 KB alloc
// into an Out-of-memory panic. The next cycle retries once the heap recovers.
#define LP_POLL_MIN_GROWABLE_B  (24u * 1024u)
static uint32_t g_core1_lp_last_poll_ms = 0;
// Operator id of the current linephone session — pinned into GET URL so the
// server can scope queries (and the POST already carries device_id).
static char     g_core1_lp_op_id[MAX_LAB_ID_LEN] = "";

//=============================================================================
// Core 1 — forward declarations
//=============================================================================
static void core1_loop(void);
static void core1_entry(void);
static void on_core1_init(void);
static void on_core1_network(void);
static void on_core1_wait_audio(void);
static void on_core1_running(void);
static void handle_core1_notify(ic_msg_t type, const uint8_t *payload, uint16_t length);

//=============================================================================
// Core 1 — dispatcher
//=============================================================================
static void core1_entry(void) {
    core1_loop();
}

static void core1_loop(void) {
    while (1) {
        ic_msg_t mtype; uint16_t mlen;
        while (ic_try_recv(&mtype, g_core1_recv_buf,
                           sizeof g_core1_recv_buf, &mlen) == 0) {
            handle_core1_notify(mtype, g_core1_recv_buf, mlen);
        }
        switch (g_core1_state) {
            case CORE1_S_INIT:       on_core1_init();       break;
            case CORE1_S_NETWORK:    on_core1_network();    break;
            case CORE1_S_WAIT_AUDIO: on_core1_wait_audio(); break;
            case CORE1_S_RUNNING:    on_core1_running();    break;
        }
    }
}

//=============================================================================
// Core 1 — state handlers
//=============================================================================
static void on_core1_init(void) {
    if (cyw43_arch_init()) {
        printf("core1: cyw43_arch_init failed\n");
        HALT();
    }
    g_core1_state = CORE1_S_NETWORK;
}

static void on_core1_network(void) {
    if (network_init(&g_wifi_creds) != 0) {
        printf("core1: network init failed\n");
        set_status("Net: init failed");
        HALT();
    }
    set_status("Net: ready");
    // STACKDIAG (B): the moment of truth for the heap budget — cyw43 + lwIP +
    // the WiFi association are all up, but no TLS handshake has run yet. The
    // largest_malloc figure here is roughly what the first altcp_tls_new has
    // to find. Compare against the encoder/decoder heap sizes to see headroom.
    stackdiag_heap_report("net-ready");
    printf("[core1] S0 done → notify core0 NET_READY\n");
    ic_send(IC_MSG_NET_READY, NULL, 0);
    g_core1_state = CORE1_S_WAIT_AUDIO;
}

static void on_core1_wait_audio(void) {
    tight_loop_contents();
}

static void on_core1_running(void) {
    static uint32_t last_dbg = 0;
    static uint32_t last_bw  = 0;
    if (last_dbg == 0) last_dbg = board_millis();
    if (last_bw  == 0) last_bw  = board_millis();

    cyw43_arch_poll();

    // HTTPS response completion
    if (g_https_state == HC_DONE_OK || g_https_state == HC_DONE_ERR) {
        https_handle_done();
    }

    // Forward decoded PCM from g_https_resp → core0 via the ic_ring
    tts_forward_pump();

    // mic_is_active(): while the user holds push-to-talk, defer ALL new
    // transport kicks (lp GET poll, lab fetch, radio/TTS POSTs, timeline).
    // A TLS handshake monopolizes core1 for ~200+ ms, during which the IC
    // ring isn't drained and mic frames get dropped ([mic] ship-block —
    // measured 10/205 frames lost on a 4 s take). In-flight transfers
    // still complete via cyw43_arch_poll; the post-release POST is not
    // affected because g_mic_active is already false by the time
    // LINEPHONE_POST_END arrives.
    bool https_busy = (g_https_state != HC_IDLE) || g_tts_play_active
                      || mic_is_active();

    // Lab-list fetch (Button-X / auto-fetch)
    if (g_core1_fetch_pending && !https_busy) {
        g_core1_fetch_pending = false;
        printf("[core1] HTTPS GET info\n");
        if (http_kick_info(g_wifi_creds.device_id) != 0) http_set_state(HC_DONE_ERR);
    }

    // Linephone (push-to-talk) — base64 + POST the captured opus stream.
    // Runs after the lab-fetch hook so it doesn't race with auto-fetch on
    // first connection, and gated on https_busy like the TTS path.
    if (g_core1_lp_post_pending && !https_busy) {
        g_core1_lp_post_pending = false;
        if (lp_kick_post(g_wifi_creds.device_id, g_core1_lp_op_id) != 0) {
            http_set_state(HC_DONE_ERR);
        }
        https_busy = true;
    }

    // Linephone GET poll — the receive side of push-to-talk. Only runs while
    // the user has entered linephone mode (Enter on a role=linephone lab);
    // outside the mode no traffic hits /api/device/linephone/. Also gated against
    // any other in-flight or pending transport.
    if (g_core1_lp_session_active
        && !https_busy
        && !g_core1_lp_post_pending
        && tts_queue_count() == 0
        && !g_core1_fetch_pending
        && !g_core1_radio_pending
        && !g_core1_tl_active) {
        uint32_t now = board_millis();
        if ((now - g_core1_lp_last_poll_ms) >= LP_POLL_INTERVAL_MS) {
            size_t growable = heap_growable();
            if (growable < LP_POLL_MIN_GROWABLE_B) {
                // Not enough heap to safely open a TLS connection — skip this
                // cycle (don't bump last_poll, so we retry on the very next
                // pass once the heap frees up). Throttled log so a sustained
                // low-heap stretch doesn't flood the UART.
                static uint32_t lp_skip_last = 0;
                if ((now - lp_skip_last) >= 1000u) {
                    lp_skip_last = now;
                    printf("[lp/get] skip poll: heap growable=%u B < %u B\n",
                           (unsigned)growable, (unsigned)LP_POLL_MIN_GROWABLE_B);
                }
            } else {
                g_core1_lp_last_poll_ms = now;
                if (lp_kick_get(g_wifi_creds.device_id, g_core1_lp_op_id) != 0) {
                    http_set_state(HC_DONE_ERR);
                } else {
                    https_busy = true;
                }
            }
        }
    }

    // Internet-radio stream start. Same /api/tts/generate_stream POST
    // as the TTS path with empty text; the server returns the radio
    // stream when the targeted operator's role=internetradio. We pin
    // operator_id to the lab the user pressed Enter on (captured into
    // g_core1_current_op_id by the RADIO_START handler) — without it
    // the server's default routing can pick any operator registered
    // for this device and return a TTS reply instead of the radio
    // stream.
    if (g_core1_radio_pending && !https_busy) {
        g_core1_radio_pending = false;
        const char *op = g_core1_current_op_id[0]
                            ? g_core1_current_op_id : NULL;
        printf("[core1] POST tts (radio kick op=%s)\n",
               op ? op : "(none)");
        if (tts_kick_request("", NULL, &g_api_ip, g_https_host,
                             g_wifi_creds.device_id,
                             g_wifi_creds.opus_enabled,
                             op) != 0) {
            http_set_state(HC_DONE_ERR);
        }
        https_busy = true;
    }

    // TTS queue processing
    if (!https_busy && tts_queue_count() > 0) {
        const char *msg    = tts_queue_peek();
        const char *gender = tts_queue_peek_gender();
        if (msg) {
            // Pin TTS to the operator we're currently polling timeline
            // for — otherwise the server's default routing picks any
            // operator on the device (including the radio one, which
            // hijacks the response into a radio stream).
            const char *op = g_core1_current_op_id[0]
                                ? g_core1_current_op_id : NULL;
            printf("[core1] POST tts (queue=%d gender=%s op=%s)\n",
                   tts_queue_count(), gender ? gender : "(default)",
                   op ? op : "(none)");
            if (tts_kick_request(msg, gender, &g_api_ip, g_https_host,
                                 g_wifi_creds.device_id,
                                 g_wifi_creds.opus_enabled,
                                 op) != 0) {
                tts_queue_pop();
                http_set_state(HC_DONE_ERR);
            }
            https_busy = true;
        }
    }

    // Timeline polling
    if (g_core1_tl_active && !https_busy && tts_queue_count() == 0) {
        uint32_t now = board_millis();
        if ((now - g_timeline_last_poll) >= TIMELINE_POLL_INTERVAL_MS) {
            g_timeline_last_poll = now;
            printf("[core1] GET timeline\n");
            if (http_kick_timeline(g_wifi_creds.device_id,
                                   g_core1_current_op_id,
                                   g_last_publish_id) != 0)
                http_set_state(HC_DONE_ERR);
        }
    }

    // Periodic state dump. Silenced during TTS to keep UART quiet — the
    // [tts/stat] aggregate already covers what we care about while streaming.
    if (g_https_state != HC_IDLE) {
        uint32_t now = board_millis();
        if ((now - last_dbg) >= 1000 && g_https_mode != HM_TTS) {
            last_dbg = now;
            printf("[core1] https_state=%d mode=%d elapsed=%lums resp_len=%u tts_q=%d play=%d\n",
                   (int)g_https_state, (int)g_https_mode,
                   (unsigned long)(now - g_https_state_at),
                   (unsigned)g_https_resp_len,
                   tts_queue_count(), (int)g_tts_play_active);
        }
    } else {
        last_dbg = board_millis();
    }

    {
        uint32_t now_bw = board_millis();
        if ((now_bw - last_bw) >= 10000) {
            uint32_t b = g_recv_bytes;
            uint32_t p = g_recv_pkts;
            g_recv_bytes = 0;
            g_recv_pkts  = 0;
            uint32_t dt  = now_bw - last_bw;
            uint32_t wire = b + p * 40;
            uint32_t kbps = wire * 8 * 1000 / dt / 1000;
            printf("[core1/bw] %us: payload=%u wire~%u (%u KB/s %u kbps) pkts=%u\n",
                   (unsigned)(dt / 1000), (unsigned)b, (unsigned)wire,
                   (unsigned)(wire * 1000 / dt / 1024), (unsigned)kbps,
                   (unsigned)p);
            last_bw = now_bw;
        }
    }

    led_blink_loop();
    led_heartbeat_loop();
    sleep_us(500);
}

//=============================================================================
// Core 1 — event handler
//=============================================================================
static void handle_core1_notify(ic_msg_t type, const uint8_t *payload, uint16_t length) {
    (void)payload; (void)length;
    switch (type) {
    case IC_MSG_AUDIO_READY:
        if (g_core1_state == CORE1_S_WAIT_AUDIO) {
            printf("[core1] S1 → entering main loop\n");
            g_core1_state = CORE1_S_RUNNING;
        }
        break;
    case IC_MSG_BTN_X:
        g_core1_fetch_pending = true;
        break;
    case IC_MSG_TIMELINE_START: {
        uint16_t n = length;
        if (n >= MAX_LAB_ID_LEN) n = MAX_LAB_ID_LEN - 1;
        memcpy(g_core1_current_op_id, payload, n);
        g_core1_current_op_id[n] = '\0';
        g_last_publish_id    = -1;
        g_timeline_last_poll = 0;
        g_core1_tl_active    = true;
        printf("[core1] TIMELINE_START operator=%s\n", g_core1_current_op_id);
        break;
    }
    case IC_MSG_TIMELINE_STOP:
        g_core1_tl_active = false;
        printf("[core1] TIMELINE_STOP\n");
        break;
    case IC_MSG_RADIO_START: {
        uint16_t n = length;
        if (n >= MAX_LAB_ID_LEN) n = MAX_LAB_ID_LEN - 1;
        memcpy(g_core1_current_op_id, payload, n);
        g_core1_current_op_id[n] = '\0';
        g_core1_radio_pending = true;
        printf("[core1] RADIO_START queued op=%s\n", g_core1_current_op_id);
        break;
    }
    case IC_MSG_RADIO_STOP:
        // Tear down the in-flight TLS pcb and flag the transport idle. The
        // forward pump then sees stream_done && source_drained and ships
        // IC_MSG_TTS_END, which drains the audio ring and flips
        // g_tts_play_active back off — same finalization path as a normal
        // TTS body ending naturally.
        printf("[core1] RADIO_STOP — cleanup pcb\n");
        g_core1_radio_pending = false;
        http_cleanup();
        http_set_state(HC_IDLE);
        break;
    case IC_MSG_TTS_PLAYED:
        if (g_tts_play_active) g_tts_play_active = false;
        tts_queue_pop();
        printf("[core1] TTS_PLAYED (queue=%d)\n", tts_queue_count());
        break;
    case IC_MSG_LINEPHONE_OPUS_PKT:
        // Mic frames only matter inside an active session — drop silently
        // otherwise (defensive: shouldn't happen because gui.c gates B on
        // g_linephone_active too).
        if (g_core1_lp_session_active) lp_accum_append(payload, length);
        break;
    case IC_MSG_LINEPHONE_POST_END:
        if (g_core1_lp_session_active) {
            printf("[core1] LINEPHONE_POST_END (accum=%u B) → queue POST\n",
                   (unsigned)lp_accum_len());
            g_core1_lp_post_pending = true;
        }
        break;
    case IC_MSG_LINEPHONE_DISCARD:
        // Talk too short to post: drop the accumulated opus so it can't
        // contaminate the next POST (or overflow g_lp_buf over repeated
        // blips). Session stays active — only the buffer is cleared.
        if (g_core1_lp_session_active) {
            printf("[core1] LINEPHONE_DISCARD (accum=%u B dropped, no POST)\n",
                   (unsigned)lp_accum_len());
            lp_accum_reset();
            g_core1_lp_post_pending = false;
        }
        break;
    case IC_MSG_LINEPHONE_START: {
        uint16_t n = length;
        if (n >= MAX_LAB_ID_LEN) n = MAX_LAB_ID_LEN - 1;
        memcpy(g_core1_lp_op_id, payload, n);
        g_core1_lp_op_id[n] = '\0';
        // Fresh session: reset cursor and accumulator so we start "from
        // the current head" (server interprets start=0 that way) and the
        // mic buffer is empty.
        g_lp_start_pos          = 0;
        g_core1_lp_last_poll_ms = 0;
        lp_accum_reset();
        g_core1_lp_session_active = true;
        printf("[core1] LINEPHONE_START op=%s\n", g_core1_lp_op_id);
        break;
    }
    case IC_MSG_LINEPHONE_STOP:
        g_core1_lp_session_active = false;
        g_core1_lp_post_pending   = false;
        g_core1_lp_op_id[0]       = '\0';
        // Drop any frames that hadn't been POSTed yet — the next session
        // start will reset anyway, but clearing now keeps state tidy if
        // the device sits idle for a while between sessions.
        lp_accum_reset();
        printf("[core1] LINEPHONE_STOP\n");
        break;
    default:
        printf("[core1] unhandled msg type=0x%02x in state=%d\n",
               type, (int)g_core1_state);
        break;
    }
}

//=============================================================================
// Core 0 — private state
//=============================================================================
static uint32_t g_time_boot = 0;

#define CORE0_BEEP_DURATION_MS 300
static uint32_t g_core0_beep_started_ms = 0;

//=============================================================================
// Core 0 — forward declarations
//=============================================================================
static void core0_loop(void);
static void on_core0_init(void);
static void on_core0_wait_net(void);
static void on_core0_audio_beep_play(void);
static void on_core0_audio_beep_wait(void);
static void on_core0_audio_test_start(void);
static void on_core0_audio_test_tick(void);
static void on_core0_audio_ready(void);
static void on_core0_running(void);
static void handle_core0_notify(ic_msg_t type, const uint8_t *payload, uint16_t length);

//=============================================================================
// Core 0 — dispatcher
//=============================================================================
static void core0_loop(void) {
    while (1) {
        ic_msg_t mtype; uint16_t mlen;
        while (ic_peek_type(&mtype) == 0) {
            // Raw-PCM back-pressure: keep the pending staging buffer from
            // overflowing.
            if (mtype == IC_MSG_TTS_PCM_CHUNK
                && g_core0_pcm_pending_len + TTS_FWD_CHUNK_SIZE
                       > TTS_PCM_PENDING_CAP) {
                break;
            }
            // Opus back-pressure: stop dequeuing opus packets when the audio
            // ring is ≥75 % full so we don't decode+write past the ring's
            // tail. Pressure cascades back through IC ring → forward_pump
            // skip → g_https_resp fill → recv_cb ERR_MEM → TCP window close,
            // throttling the server without ever blocking core1's main loop.
            if (mtype == IC_MSG_TTS_OPUS_PKT
                && audio_stream_buffered() > (BUF_FRAMES * 3) / 4) {
                // Throttled diagnostic: confirms the dispatcher is
                // back-pressuring (vs. some other stall). Pair with the
                // [tts/opus] ship-block log on core1 to localize wedges.
                static uint32_t last_drain_log = 0;
                uint32_t now_drain = board_millis();
                if ((now_drain - last_drain_log) >= 500) {
                    last_drain_log = now_drain;
                    printf("[core0] opus drain paused: buffered=%u/%u "
                           "(threshold=%u)\n",
                           (unsigned)audio_stream_buffered(),
                           (unsigned)BUF_FRAMES,
                           (unsigned)((BUF_FRAMES * 3) / 4));
                }
                break;
            }
            ic_try_recv(&mtype, g_core0_recv_buf,
                        sizeof g_core0_recv_buf, &mlen);
            handle_core0_notify(mtype, g_core0_recv_buf, mlen);
        }
        switch (g_core0_state) {
            case CORE0_S_INIT:             on_core0_init();             break;
            case CORE0_S_WAIT_NET:         on_core0_wait_net();         break;
            case CORE0_S_AUDIO_BEEP_PLAY:  on_core0_audio_beep_play();  break;
            case CORE0_S_AUDIO_BEEP_WAIT:  on_core0_audio_beep_wait();  break;
            case CORE0_S_AUDIO_TEST_START: on_core0_audio_test_start(); break;
            case CORE0_S_AUDIO_TEST_TICK:  on_core0_audio_test_tick();  break;
            case CORE0_S_AUDIO_READY:      on_core0_audio_ready();      break;
            case CORE0_S_RUNNING:          on_core0_running();          break;
        }
    }
}

//=============================================================================
// Core 0 — state handlers
//=============================================================================
static void on_core0_init(void) {
    printf("[core0] S0: waiting for NET_READY from core1...\n");
    g_core0_state = CORE0_S_WAIT_NET;
}

static void on_core0_wait_net(void) {
    tight_loop_contents();
}

static void on_core0_audio_beep_play(void) {
    printf("[core0] NET_READY received → audio_init + boot beep\n");
    audio_init();
    // INMP441 capture pipeline init. Claims pio0 SM + a DMA channel + the
    // Opus encoder (heap). Pins stay as plain GPIO (buttons) until
    // mic_start() flips them to PIO at the first Button-B press.
    if (mic_init() != 0) {
        printf("[core0] mic_init failed — push-to-talk disabled\n");
    }
    // HC-SR04 on pio1 (pio0=mic, pio2=cyw43+audio). Claims one SM; the trigger
    // pulse and echo timing run on the SM, so the main loop never busy-waits.
    hc_sr04_init(&g_sonar, SONAR_PIO, SONAR_TRIG_PIN, SONAR_ECHO_PIN);
    // Proximity LED on a plain GPIO (active-high), off until something is near.
    gpio_init(SONAR_LED_PIN);
    gpio_set_dir(SONAR_LED_PIN, GPIO_OUT);
    gpio_put(SONAR_LED_PIN, 0);
    printf("[core0] HC-SR04 init: pio1 TRIG=GP%d ECHO=GP%d LED=GP%d (<%.0f cm)\n",
           SONAR_TRIG_PIN, SONAR_ECHO_PIN, SONAR_LED_PIN, (double)SONAR_NEAR_CM);
    audio_play_sine();
    g_core0_beep_started_ms = board_millis();
    g_core0_state = CORE0_S_AUDIO_BEEP_WAIT;
}

static void on_core0_audio_beep_wait(void) {
    if (board_millis() - g_core0_beep_started_ms >= CORE0_BEEP_DURATION_MS) {
        audio_stop();
        printf("[core0] startup beep done → arming streaming test\n");
        g_core0_state = CORE0_S_AUDIO_TEST_START;
    }
}

static void on_core0_audio_test_start(void) {
    audio_test_stream_sine_begin();
    g_core0_state = CORE0_S_AUDIO_TEST_TICK;
}

static void on_core0_audio_test_tick(void) {
    if (audio_test_stream_sine_tick() == AUDIO_TEST_DONE) {
        g_core0_state = CORE0_S_AUDIO_READY;
    }
}

// One-shot: armed when the startup auto-fetch is triggered, consumed by the
// first IC_MSG_LABS_READY. Gates the single-lab auto-pilot to the boot fetch
// only — manual Button-X re-fetches never auto-press Enter.
static bool g_autopilot_armed = false;
// Set by the LABS_READY handler when the boot fetch yields a single lab;
// executed in on_core0_running (the main-loop body). The Enter action must
// NOT run inside handle_core0_notify: it issues a blocking ic_send, and doing
// that from within the FIFO drain loop can two-way-deadlock with core1.
static bool g_autopilot_pending = false;

static void on_core0_audio_ready(void) {
    printf("[core0] S0 done → notify core1 AUDIO_READY\n");
    ic_send(IC_MSG_AUDIO_READY, NULL, 0);
    g_core0_state = CORE0_S_RUNNING;
}

static void on_core0_running(void) {
    static uint32_t last_log = 0;
    static uint32_t last_status_ver = 0;
    static bool auto_fetched = false;
    if (last_log == 0) last_log = board_millis();

    buttons_poll();

    // Deferred single-lab auto-pilot (armed by the startup auto-fetch). Run
    // here in the main-loop body — same context as a manual Enter via
    // buttons_poll — because lab_enter_action() issues a blocking ic_send.
    // The active-state guard keeps it a pure auto-start, never an auto-stop.
    if (g_autopilot_pending) {
        g_autopilot_pending = false;
        if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count == 1
            && !g_timeline_active && !g_linephone_active && !g_tts_play_active) {
            g_lab_selected = 0;
            printf("[core0] auto-pilot: single lab, pressing Enter\n");
            lab_enter_action();
        }
    }

    // Drain captured PCM from the INMP441 DMA ring, encode 20 ms frames,
    // and ship each opus packet to core1 via IC_MSG_LINEPHONE_OPUS_PKT.
    // No-op when mic is not active (Button-B not held).
    mic_pump();

    // Drain PCM from core1 into the audio ring buffer
    tts_play_pump();

    // Redraw status view when a new line arrives
    if (g_view == VIEW_STATUS) {
        uint32_t ver = gui_status_version();
        if (ver != last_status_ver) {
            last_status_ver = ver;
            render_status_view();
        }
    }

    // Auto-fetch lab list once network comes up
    if (!auto_fetched) {
        auto_fetched = true;
        g_autopilot_armed = true;
        mem_barrier();
        printf("[core0] auto-fetch triggered\n");
        trigger_fetch();
    }

    uint32_t now = board_millis();

    // HC-SR04 distance: fire one ping per interval, read the prior result
    // non-blocking (PIO does the timing — no busy-wait here). read returns
    // false until a result is ready; <0 cm means out of range. Log ONLY on a
    // meaningful change (out-of-range edge, or >= SONAR_LOG_DELTA_CM move) so
    // a static scene doesn't flood the UART at 10 Hz.
    static uint32_t sonar_last = 0;
    static bool     sonar_near = false;      // latest proximity (<SONAR_NEAR_CM)
    // static int   sonar_last_oor = -1;     // (was) log-throttle state
    // static float sonar_last_cm  = -1.0f;  // (was) last logged distance
    if ((now - sonar_last) >= SONAR_PING_INTERVAL_MS) {
        sonar_last = now;
        float cm;
        if (hc_sr04_read(&g_sonar, &cm)) {
            // --- sonar logging suppressed (uncomment to re-enable) ---
            // if (cm < -1.5f) {           // -2.0 → ECHO stuck high
            //     if (sonar_last_oor != 2)
            //         printf("[sonar] ECHO STUCK HIGH — divider too high / ECHO floating / short\n");
            //     sonar_last_oor = 2;
            // } else if (cm < 0) {        // -1.0 → ECHO never rose
            //     if (sonar_last_oor != 1)
            //         printf("[sonar] NO ECHO RISE — trigger not reaching sensor / no 5V VCC / ECHO open / divider below V_IH\n");
            //     sonar_last_oor = 1;
            // } else {
            //     if (sonar_last_oor != 0
            //         || fabsf(cm - sonar_last_cm) >= SONAR_LOG_DELTA_CM) {
            //         printf("[sonar] %.1f cm\n", cm);
            //         sonar_last_cm = cm;
            //     }
            //     sonar_last_oor = 0;
            // }
            // Latest proximity status (drives the mic SM; the LED follows the
            // mic ACTIVE window below, NOT raw proximity — otherwise it stays
            // lit while the object lingers after the 3 s auto-POST).
            // A sentinel (cm < 0 → no-rise/stuck) counts as "nothing near".
            sonar_near = (cm >= 0.0f && cm < SONAR_NEAR_CM);
        }
        hc_sr04_trigger(&g_sonar);
    }

    // Sonar proximity push-to-talk (runs every loop). Proximity acts like a
    // held Button-B via gui_button_b(): mic ON when near, forced OFF after
    // SONAR_MIC_MAX_MS (or when the object leaves first), then a
    // SONAR_MIC_COOLDOWN_MS lockout. It will NOT re-arm until the object has
    // first moved away (sonar_near goes false) — one ON→OFF per approach.
    static enum { SONAR_MIC_IDLE, SONAR_MIC_ACTIVE, SONAR_MIC_COOLDOWN }
        sonar_mic = SONAR_MIC_IDLE;
    static uint32_t sonar_mic_since = 0;
    switch (sonar_mic) {
        case SONAR_MIC_IDLE:
            if (sonar_near) {
                gui_button_b(true);                 // mic ON  (== Button-B press)
                sonar_mic = SONAR_MIC_ACTIVE;
                sonar_mic_since = now;
            }
            break;
        case SONAR_MIC_ACTIVE:
            if (!sonar_near || (now - sonar_mic_since) >= SONAR_MIC_MAX_MS) {
                // POST only if the mic ran long enough; otherwise discard so a
                // quick swipe doesn't upload a near-empty clip.
                bool do_post = (now - sonar_mic_since) >= SONAR_MIC_MIN_POST_MS;
                gui_ptt_stop(do_post);              // mic OFF (POST or DISCARD)
                sonar_mic = SONAR_MIC_COOLDOWN;
                sonar_mic_since = now;
            }
            break;
        case SONAR_MIC_COOLDOWN:
            // Re-arm only after the lockout AND once the object has left, so a
            // single approach yields exactly one ON→OFF (no 3 s/1 s cycling).
            if ((now - sonar_mic_since) >= SONAR_MIC_COOLDOWN_MS && !sonar_near)
                sonar_mic = SONAR_MIC_IDLE;
            break;
    }

    // Proximity LED tracks the capture window, not raw proximity: it lights
    // when the mic is ACTIVE and goes dark the instant the mic is forced OFF
    // by the 3 s auto-POST (or an early leave), even if the object is still
    // within range. Re-lights only on the next approach (one ON→OFF per
    // approach), matching the mic SM.
    gpio_put(SONAR_LED_PIN, sonar_mic == SONAR_MIC_ACTIVE);

    // Re-paint the LAB list when polling state flips
    static bool last_tl_active_ui = false;
    if (g_timeline_active != last_tl_active_ui) {
        last_tl_active_ui = g_timeline_active;
        if (g_view == VIEW_LABS && g_lab_state == LAB_OK) render_lab_list();
    }

    // Spinner + speaking icon on the LABS view
    static uint32_t spinner_last  = 0;
    static uint32_t spinner_phase = 0;
    static uint32_t fast_last     = 0;
    static bool     fast_blink    = false;
    static int last_tl_status_ver = 0;
    if ((now - spinner_last) >= 200) {
        spinner_last = now;
        if (g_timeline_active) spinner_phase++;
        if (g_timeline_status_ver != last_tl_status_ver && g_view == VIEW_LABS) {
            last_tl_status_ver = g_timeline_status_ver;
            set_status("TL: pid=%lld q=%d", (long long)g_last_publish_id,
                       tts_queue_count());
        }
    }
    if ((now - fast_last) >= 100) {
        fast_last = now;
        fast_blink = !fast_blink;
    }
    if (g_view == VIEW_LABS && (g_timeline_active || g_tts_play_active)) {
        render_lab_spinner(spinner_phase, fast_blink);
    }

    // Periodic debug log. Silenced while a TTS request is in flight (or
    // playback is active) so the UART doesn't compete with audio cadence.
    bool tts_busy = g_tts_play_active
                 || (g_https_state != HC_IDLE && g_https_mode == HM_TTS);
    if ((now - last_log) >= 1000 && !tts_busy) {
        last_log = now;
        printf("[core0] t=%lu ms view=%d lab_state=%d count=%d sel=%d tl_active=%d\n",
               (unsigned long)now, (int)g_view, (int)g_lab_state,
               g_lab_count, g_lab_selected, (int)g_timeline_active);
    }

    // STACKDIAG (C): peak core0 stack usage so far. Own timer (prints even
    // while tts_busy / recording — that's exactly when the encoder/decoder
    // push the stack deepest). Monotonic, so the max after a record+playback
    // cycle is the number we size PICO_STACK_SIZE against.
    static uint32_t last_sd = 0;
    if ((now - last_sd) >= 2000) {
        last_sd = now;
        size_t used  = stackdiag_hiwater();
        size_t total = (size_t)((uintptr_t)__StackTop - (uintptr_t)__StackBottom);
        if (used > total) {
            // Peak ran past __StackBottom into the heap-growth gap. "spill"
            // is how far — the number PICO_STACK_SIZE is short by. If spill
            // pegs at the probe window (gap paint fully consumed), the true
            // peak is even deeper: ">= spill".
            printf("[stackdiag] core0 stack OVERFLOW hiwater=%u B region=%u "
                   "spill=%u B (probe window %u) brk=0x%08x mic=%d\n",
                   (unsigned)used, (unsigned)total, (unsigned)(used - total),
                   (unsigned)((uintptr_t)__StackBottom - stackdiag_floor()),
                   (unsigned)(uintptr_t)sbrk(0), (int)mic_is_active());
            // Fingerprint the writer: dump 8 words at the deepest non-canary
            // point. Real stack frames carry flash return addresses
            // (0x10xxxxxx) and RAM pointers (0x20xxxxxx); a rogue audio/DMA
            // writer leaves 16-bit-sample-looking words instead.
            const uint32_t *deep = (const uint32_t *)
                ((uintptr_t)__StackTop - used);
            printf("[stackdiag] @%p:", (const void *)deep);
            for (int i = 0; i < 8; i++) printf(" %08x", (unsigned)deep[i]);
            printf("\n");
        } else {
            printf("[stackdiag] core0 stack hiwater=%u/%u B (free=%u) mic=%d\n",
                   (unsigned)used, (unsigned)total, (unsigned)(total - used),
                   (int)mic_is_active());
        }
    }
    // 1 ms (was 10 ms): the previous cadence let the IC ring fill for up to
    // ~10 ms before tts_play_pump drained it, which made core1's ic_send block
    // on a full ring → opus decode latency ballooned (max_us hit 265 ms in
    // [tts/stat]). 1 ms keeps PCM draining promptly without burning the core.
    sleep_us(1000);
}

//=============================================================================
// Core 0 — event handler
//=============================================================================
static void handle_core0_notify(ic_msg_t type, const uint8_t *payload, uint16_t length) {
    switch (type) {
    case IC_MSG_NET_READY:
        if (g_core0_state == CORE0_S_WAIT_NET) {
            g_core0_state = CORE0_S_AUDIO_BEEP_PLAY;
        }
        break;
    case IC_MSG_LABS_READY:
        if (g_view == VIEW_LABS) {
            if (g_lab_state == LAB_OK)        render_lab_list();
            else if (g_lab_state == LAB_ERR)  render_lab_error();
        }
        // Auto-pilot: on the startup auto-fetch only, when the list resolves
        // to exactly one lab, request an automatic Enter on it. The
        // g_autopilot_armed one-shot keeps this off for manual Button-X
        // re-fetches. We only flag it here and let on_core0_running run the
        // Enter action outside the FIFO drain loop (see g_autopilot_pending).
        if (g_autopilot_armed) {
            g_autopilot_armed = false;
            if (g_lab_state == LAB_OK && g_lab_count == 1) {
                g_autopilot_pending = true;
            }
        }
        break;
    case IC_MSG_TTS_PCM_CHUNK:
        if (g_core0_pcm_pending_len + length > TTS_PCM_PENDING_CAP) {
            printf("[core0] PCM_CHUNK overflow (have=%u +%u cap=%u) — dropping\n",
                   (unsigned)g_core0_pcm_pending_len, length,
                   (unsigned)TTS_PCM_PENDING_CAP);
            break;
        }
        memcpy(g_core0_pcm_pending + g_core0_pcm_pending_len, payload, length);
        g_core0_pcm_pending_len += length;
        break;
    case IC_MSG_TTS_OPUS_PKT: {
        // Opus packets arrive in stream order from core1's forward_pump.
        // Decode here (core0) so core1 stays free for cyw43_arch_poll / TLS,
        // and write straight into the audio ring. The dispatcher's
        // audio_stream_buffered() > 75 % gate prevents this from racing past
        // the DMA tail; on overflow audio_stream_write_mono16 just returns
        // partial, but the gate makes that path unreachable in practice.
        //
        // pcm[] is static, kept off core0's stack. This was once a hard
        // requirement: core0's stack was 4 KB (SCRATCH_Y) and libopus's SILK
        // WB 20 ms fixed-point decoder put ~3 KB of scratch on the stack as
        // VLAs, so a stack-allocated 960 B pcm[] overflowed it — corrupting
        // return addresses and silently wedging the dispatcher (audio
        // underruns forever, no further [tts/opus] forward log lines because
        // core1's ic_send blocks on the FIFO that core0 stops draining). That
        // hazard is gone now — core0's stack is 20 KB of SCRATCH_X-annexed RAM
        // (memmap_bigstack.ld) and the codec scratch lives in the BSS
        // pseudostack (src/opus_scratch.c), not stack VLAs — but static stays:
        // it's safe because handle_core0_notify is the sole consumer and runs
        // single-threaded on core0, and it keeps this buffer off the frame.
        static int16_t pcm[OPUS_STREAM_FRAME_SAMPLES];
        int n = opus_stream_decode_frame(payload, length,
                                         pcm, OPUS_STREAM_FRAME_SAMPLES);
        if (n > 0) audio_stream_write_mono16(pcm, (size_t)n);
        break;
    }
    case IC_MSG_TTS_END:
        g_core0_tts_stream_done = true;
        printf("[core0] TTS_END (pending=%u)\n",
               (unsigned)g_core0_pcm_pending_len);
        break;
    default:
        printf("[core0] unhandled msg type=0x%02x in state=%d\n",
               type, (int)g_core0_state);
        break;
    }
}

//=============================================================================
// Read the onboard BOOTSEL button at runtime. There is no GPIO for it: the
// button sits on the QSPI chip-select line, so we momentarily drive that pad's
// output-enable low (releasing the flash CS), sample the pin through SIO, then
// restore it. The flash can't be accessed while CS is perturbed, so this whole
// routine MUST run from RAM (__no_inline_not_in_flash_func) with interrupts
// disabled — otherwise an XIP fetch mid-sample would hang the core. Returns
// true while the button is held. (Standard pico-sdk idiom; RP2350 reads the
// CS state from SIO_GPIO_HI_IN_QSPI_CSN rather than a GPIO bit.)
static bool __no_inline_not_in_flash_func(read_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Brief settle so the pad reflects the (un)pressed level before we sample.
    for (volatile int i = 0; i < 1000; ++i) tight_loop_contents();

    // Pressed pulls CS low → invert. RP2350 surfaces the QSPI CS input here.
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

// Rapid LED flash (~1.3 s) on the CYW43 onboard LED. Used as the "entering
// provisioning" signal — requires cyw43_arch_init() to have already succeeded.
static void led_fast_blink(void) {
    for (int i = 0; i < 16; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i & 1);
        sleep_ms(80);
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

// Visible BOOTSEL provisioning window. The onboard LED lives on the CYW43
// chip, so this requires cyw43_arch_init() to have already succeeded. We blink
// the LED slowly five times (~2.5 s) to give the operator an obvious moment to
// tap BOOTSEL on a display-less board, polling the button often so a brief tap
// isn't missed. Returns true on a hit (caller enters provisioning, which does
// the fast-blink confirmation). NOTE: BOOTSEL must NOT be held from the instant
// of power-up — that lands in the bootrom's UF2 mass-storage mode. Power on
// first, wait for the slow blink, then tap.
static bool run_bootsel_prov_window(void) {
    const int      SLOW_BLINKS = 5;
    const uint32_t HALF_MS     = 250;  // LED half-period (slow, visible)
    const uint32_t POLL_MS     = 10;   // BOOTSEL sample cadence
    bool pressed = false;

    printf("[boot] BOOTSEL window: tap to enter provisioning\n");
    for (int b = 0; b < SLOW_BLINKS && !pressed; b++) {
        for (int phase = 0; phase < 2 && !pressed; phase++) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, phase == 0 ? 1 : 0);
            for (uint32_t t = 0; t < HALF_MS && !pressed; t += POLL_MS) {
                if (read_bootsel_button()) pressed = true;
                sleep_ms(POLL_MS);
            }
        }
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    if (pressed) printf("[boot] BOOTSEL detected — entering provisioning\n");
    return pressed;
}

// Provisioning bootmode — runs entirely on core0, never returns. On
// successful Commit we save to flash and reboot so the normal boot path
// picks up the new creds. We deliberately don't launch core1 here so the
// flash write doesn't have to fight WiFi/lwIP for the bus. cyw43_ready means
// the caller already ran cyw43_arch_init() (for the BOOTSEL window) and we
// must not init it twice.
//=============================================================================
static void run_provisioning_and_reboot(bool cyw43_ready) {
    set_status("BLE prov mode");
    render_status_view();

    if (!cyw43_ready && cyw43_arch_init()) {
        printf("[prov] cyw43_arch_init failed\n");
        set_status("BLE: init fail");
        HALT();
    }

    // Fast-blink the onboard LED right before we enter provisioning — a single
    // consistent signal for every trigger (GP21 / BOOTSEL / no-creds). cyw43 is
    // guaranteed up by this point.
    led_fast_blink();

    wifi_creds_t creds = {0};
    // 0 = no timeout — user holds the device until done.
    if (ble_provision_run(&creds, 0) != 0) {
        printf("[prov] aborted\n");
        set_status("BLE: aborted");
        HALT();
    }

    // Switch back to the status log view so the user sees save/reboot
    // progress — the provisioning view (all checkmarks) hides further
    // set_status output otherwise.
    set_status("Saving to flash...");
    render_status_view();

    if (creds_save(&creds) != 0) {
        set_status("Flash save fail");
        render_status_view();
        HALT();
    }
    set_status("Saved. Reboot...");
    render_status_view();
    sleep_ms(1500);
    watchdog_reboot(0, 0, 100);
    while (1) tight_loop_contents();
}

//=============================================================================
// Entry point (runs on Core 0)
//=============================================================================
int main() {
    // 200 MHz overclock. RP2350's default 150 MHz leaves the libopus decoder
    // right at realtime for 16 kHz/16 kbps Opus on dense music frames
    // (CELT/Hybrid mode), eating all headroom; [tts/stat] cpu_ms pinned ~1000
    // and any spike caused an audio underrun. Bumping vreg to 1.15 V before
    // the clock change keeps the PLL stable above the default 1.10 V rail.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    set_sys_clock_khz(200000, true);

    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    stdio_init_all();

    sleep_ms(2000);
    g_time_boot = board_millis();
    printf("\n\n=== RP2350 HTTPS + PicoLCD-1.3 ===\n");
    printf("[TIMING] Boot complete: %lu ms\n", (unsigned long)g_time_boot);

    // STACKDIAG (C): paint core0's stack now, while it's shallow, so the
    // high-water scan in on_core0_running() can report the deepest the opus
    // encoder/decoder ever pushed it. Measurement only — no behavior change.
    stackdiag_paint();
    printf("[stackdiag] stack region: bottom=0x%08x top=0x%08x total=%u B\n",
           (unsigned)(uintptr_t)__StackBottom, (unsigned)(uintptr_t)__StackTop,
           (unsigned)((uintptr_t)__StackTop - (uintptr_t)__StackBottom));

    lcd_init();
    buttons_init();
    // GP21 (PROV_BUTTON_PIN) is wired to INMP441 SD post-boot. buttons_init
    // no longer touches it (the entry is commented out in g_buttons[]), so
    // explicitly configure it here as a pulled-up GPIO input for the boot-
    // time BLE-provisioning trigger. mic_start() will reassign it to the
    // PIO function later.
    gpio_init(PROV_BUTTON_PIN);
    gpio_set_dir(PROV_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PROV_BUTTON_PIN);
    set_status("Boot %lums", (unsigned long)g_time_boot);
    render_status_view();

    // Determine bootmode:
    //  - GP21 held LOW at boot          → force BLE provisioning (display Y-btn)
    //  - BOOTSEL tapped in the window   → force BLE provisioning (headless board)
    //  - No valid creds in flash        → fall through to provisioning
    //  - Otherwise                      → normal WiFi/HTTPS boot
    sleep_ms(50);  // let GPIO pulls settle
    bool button_held = (gpio_get(PROV_BUTTON_PIN) == 0);
    bool have_creds  = (creds_load(&g_wifi_creds) == 0);

    // The BOOTSEL window needs the onboard LED, which is on the CYW43 chip, so
    // bring cyw43 up here on core0 just for the window. If we end up provisioning
    // we keep it (provisioning runs on core0); for a normal boot we hand it back
    // with cyw43_arch_deinit() so core1 can own it cleanly (threadsafe-background
    // installs its async_context on whichever core calls init).
    bool cyw43_ready = (cyw43_arch_init() == 0);
    if (!cyw43_ready) printf("[boot] cyw43 init failed — skipping BOOTSEL window\n");

    bool bootsel_tapped = cyw43_ready && run_bootsel_prov_window();
    bool want_prov = button_held || bootsel_tapped || !have_creds;
    printf("[boot] gp21=%d bootsel=%d have_creds=%d -> prov=%d\n",
           button_held, bootsel_tapped, have_creds, want_prov);

    if (want_prov) {
        set_status(button_held ? "BLE: forced"
                   : bootsel_tapped ? "BLE: BOOTSEL"
                   : "BLE: no creds");
        run_provisioning_and_reboot(cyw43_ready);  // never returns
    }

    // Normal boot: release cyw43 on core0 so core1 re-inits and owns it.
    if (cyw43_ready) cyw43_arch_deinit();

    ic_init();
    printf("Launching core1 for CYW43/WiFi/HTTPS...\n");
    multicore_launch_core1(core1_entry);

    core0_loop();
    return 0;
}
