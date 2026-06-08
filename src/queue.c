#include "queue.h"

#include <stdio.h>
#include <string.h>

static inline void mem_barrier(void) {
    __asm__ volatile("dmb" ::: "memory");
}

static char         g_tts_queue[TTS_QUEUE_SIZE][TTS_MSG_LEN];
static char         g_tts_gender[TTS_QUEUE_SIZE][TTS_GENDER_LEN];
static volatile int g_tts_head = 0;   // read (next to dequeue)
static volatile int g_tts_tail = 0;   // write (next free slot)

int tts_queue_count(void) {
    return (g_tts_tail - g_tts_head + TTS_QUEUE_SIZE * 2) % TTS_QUEUE_SIZE;
}

bool tts_queue_push(const char *msg, const char *gender) {
    if (!msg || !*msg) return false;
    int next = (g_tts_tail + 1) % TTS_QUEUE_SIZE;
    if (next == g_tts_head) {
        printf("[tts] queue full, dropping: %.40s\n", msg);
        return false;
    }
    strncpy(g_tts_queue[g_tts_tail], msg, TTS_MSG_LEN - 1);
    g_tts_queue[g_tts_tail][TTS_MSG_LEN - 1] = '\0';
    if (gender && *gender) {
        strncpy(g_tts_gender[g_tts_tail], gender, TTS_GENDER_LEN - 1);
        g_tts_gender[g_tts_tail][TTS_GENDER_LEN - 1] = '\0';
    } else {
        g_tts_gender[g_tts_tail][0] = '\0';
    }
    mem_barrier();
    g_tts_tail = next;
    printf("[tts] queued (#%d, gender=%s): %.60s\n",
           tts_queue_count(),
           (gender && *gender) ? gender : "(default)",
           msg);
    return true;
}

const char *tts_queue_peek(void) {
    if (g_tts_head == g_tts_tail) return NULL;
    return g_tts_queue[g_tts_head];
}

const char *tts_queue_peek_gender(void) {
    if (g_tts_head == g_tts_tail) return NULL;
    if (g_tts_gender[g_tts_head][0] == '\0') return NULL;
    return g_tts_gender[g_tts_head];
}

void tts_queue_pop(void) {
    if (g_tts_head == g_tts_tail) return;
    g_tts_head = (g_tts_head + 1) % TTS_QUEUE_SIZE;
}
