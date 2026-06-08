#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// DMA ring size in stereo frames. Streaming clients use this to size an
// end-of-stream silence pad that fully wipes the ring (ENDLESS DMA keeps
// reading the last words otherwise — the "chi-chi-chi" tail loop).
#define BUF_FRAMES  4096

void audio_init(void);
void audio_play_sine(void);
void audio_stop(void);
bool audio_is_ready(void);

// Pause/resume I2S output without touching the ring or the upstream decoder.
// Used by the push-to-talk path: B-press pauses playback so the mic capture
// isn't drowned out by the speaker, and B-release resumes from where the
// DMA left off. Decode-side throughput naturally throttles via the existing
// 75 %-full back-pressure cascade while paused.
void audio_pause(void);
void audio_resume(void);
bool audio_is_paused(void);

// Stream a sine wave through audio_stream_write_mono16() — exercises the same
// ring/DMA path that TTS uses. If this is silent, the streaming write path is
// the culprit; if it plays, TTS silence is a payload/format issue.
//
// One-shot blocking variant (legacy; used by ad-hoc test scripts only):
void audio_test_stream_sine(void);

// Tick-driven variant for cooperative state machines.
// _begin: arm the test (resets sample rate + ring, clears progress state).
// _tick:  attempt to push the next batch into the ring; returns DONE when the
//         full sine sequence + silence pad has been written. Non-blocking;
//         when the ring is full the call simply returns RUNNING and the
//         caller should call again on its next event-loop tick.
typedef enum {
    AUDIO_TEST_RUNNING = 0,
    AUDIO_TEST_DONE    = 1,
} audio_test_status_t;
void                 audio_test_stream_sine_begin(void);
audio_test_status_t  audio_test_stream_sine_tick(void);

// Change PIO clkdiv to match a new PCM sample rate. Safe to call anytime.
void audio_set_sample_rate(uint32_t hz);

// SPSC streaming: push 16-bit signed mono samples (host endian); each sample
// is duplicated to L/R inside the audio ring. Returns samples accepted (<= n).
size_t audio_stream_write_mono16(const int16_t *samples, size_t n);

// Samples currently buffered ahead of the DMA read pointer.
size_t audio_stream_buffered(void);

// Streaming clients call this once per loop iteration to detect+recover
// from "DMA passed the writer" underruns. Returns true if it just zeroed
// the ring (producer should treat the buffer as freshly empty). Direct-
// fill callers (audio_play_sine) must NOT call this.
bool audio_stream_underrun_recover(void);

// Zero the ring and snap the write pointer to the current read pointer so
// the next write_mono16 begins playing immediately (no stale audio first).
void audio_stream_reset(void);
