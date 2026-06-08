// FIFO ring of pending TTS messages (UTF-8 text).
//
// Producer/consumer split:
//   - Producer (push): currently called from both cores (core0 button-A debug
//     trigger, core1 timeline-poll handler).
//   - Consumer (peek/pop): core1 only, from the HTTPS request kickoff path.
//
// Single-slot writer/reader on each side → SPSC; head/tail are volatile and
// the producer issues a DMB before publishing the new tail so the payload
// store lands first. No spinlock.
#pragma once
#include <stdbool.h>

#define TTS_QUEUE_SIZE  8
#define TTS_MSG_LEN     512
#define TTS_GENDER_LEN  16

// Returns the number of pending messages (0..TTS_QUEUE_SIZE-1).
int tts_queue_count(void);

// Returns true if the message was queued, false if dropped (queue full or
// empty/NULL message). `gender` may be NULL or "" — caller chooses the
// default at the TTS request site.
bool tts_queue_push(const char *msg, const char *gender);

// Returns a pointer to the head message, or NULL if the queue is empty.
// Pointer remains valid until the next tts_queue_pop().
const char *tts_queue_peek(void);

// Returns a pointer to the head message's gender ("male"/"female"/...),
// or NULL if the queue is empty or no gender was supplied at push time.
// Pointer remains valid until the next tts_queue_pop().
const char *tts_queue_peek_gender(void);

// Removes the head message. No-op when the queue is empty.
void tts_queue_pop(void);
