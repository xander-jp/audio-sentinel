// INMP441 → I2S RX (PIO+DMA) → 16 kHz mono int16 PCM → Opus encoder.
// Encoded packets are shipped to core1 via IC_MSG_LINEPHONE_OPUS_PKT, where
// they are accumulated, base64-encoded, and POSTed to /api/device/linephone/.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MIC_SAMPLE_RATE_HZ   16000
// 20 ms @ 16 kHz = 320 samples — matches the playback Opus frame size and
// keeps the encoder running at the lowest CBR bitrate we negotiate (8 kbps).
#define MIC_FRAME_SAMPLES    320

// One-shot init. Claims PIO+DMA+opus encoder. Idempotent.
// Pins are wired per the board: BCK=GP16, LRCK=GP20, SD=GP21.
// Returns 0 on success, -1 on failure.
int  mic_init(void);

// Start capture. Resets the encoder state, configures GP16/20/21 as I2S
// (overriding their Waveshare button assignments for the duration of the
// capture), and arms the DMA / PIO. Subsequent mic_pump() calls will encode
// and ship packets.
void mic_start(void);

// Stop capture. Halts PIO+DMA, restores GP16/20/21 to GPIO with pull-ups so
// the buttons work again, and flushes any partial PCM frame.
void mic_stop(void);

// True between mic_start() and mic_stop().
bool mic_is_active(void);

// Drain captured PCM, encode complete 20 ms frames into opus, and ship each
// packet to core1 via IC_MSG_LINEPHONE_OPUS_PKT. Call once per core0 main
// loop iteration while capturing.
void mic_pump(void);
