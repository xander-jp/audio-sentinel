#include "gui.h"
#include "common.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pico/mutex.h"
#include "st7789.h"
#include "ic_ring.h"
#include "audio.h"
#include "mic.h"

//=============================================================================
// Shared state (owned here, externed in gui.h)
//=============================================================================
volatile view_state_t g_view = VIEW_STATUS;
volatile lab_state_t  g_lab_state    = LAB_IDLE;
lab_entry_t g_labs[MAX_LABS];
int         g_lab_count    = 0;
int         g_lab_selected = 0;

bool          g_timeline_active    = false;
char          g_timeline_lab_id[MAX_LAB_ID_LEN] = "";
volatile int  g_timeline_status_ver = 0;
volatile bool g_tts_play_active = false;

bool          g_linephone_active   = false;
char          g_linephone_lab_id[MAX_LAB_ID_LEN] = "";

//=============================================================================
// Status log (cross-core mini log on LCD)
//=============================================================================
#define STATUS_LINES    9
#define STATUS_LINE_LEN 24
#define STATUS_FONT     2
#define STATUS_ROW_H    22
#define STATUS_ROW_Y0   28

static char g_status_lines[STATUS_LINES][STATUS_LINE_LEN];
static int  g_status_head  = 0;
static int  g_status_count = 0;
static volatile uint32_t g_status_version = 0;
auto_init_mutex(g_status_mutex);

void set_status(const char *fmt, ...) {
    mutex_enter_blocking(&g_status_mutex);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status_lines[g_status_head], STATUS_LINE_LEN, fmt, ap);
    va_end(ap);
    int written = g_status_head;
    g_status_head = (g_status_head + 1) % STATUS_LINES;
    if (g_status_count < STATUS_LINES) g_status_count++;
    g_status_version++;
    mutex_exit(&g_status_mutex);

    printf("[status] %s\n", g_status_lines[written]);
}

uint32_t gui_status_version(void) {
    return g_status_version;
}

//=============================================================================
// Status view
//=============================================================================
void render_status_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Boot", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);

    mutex_enter_blocking(&g_status_mutex);
    int n = g_status_count;
    int start = (g_status_head - n + STATUS_LINES) % STATUS_LINES;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % STATUS_LINES;
        int y = STATUS_ROW_Y0 + i * STATUS_ROW_H;
        lcd_draw_text(TEXT_X, y, g_status_lines[idx], STATUS_FONT, COLOR_WHITE, COLOR_BLACK);
    }
    mutex_exit(&g_status_mutex);
}

//=============================================================================
// Buttons (Waveshare Pico-LCD-1.3)
//=============================================================================
typedef struct {
    const char *name;
    uint8_t     pin;
    bool        pressed;
} button_t;

// GP16 / GP20 / GP21 are repurposed as INMP441 I2S RX pins (BCK / LRCK / SD)
// and are NOT polled here. Reading them during a recording session would
// generate phantom PRESS/RELEASE transitions at the BCK / LRCK clock rates.
// Entries kept commented out so the index alignment with gui_button_id_t
// stays obvious if we ever rewire them back to buttons.
static button_t g_buttons[] = {
    {"Button-A",  15, false},
    {"Button-B",  17, false},
    {"Button-X",  19, false},
    // {"Button-Y",  21, false},   // GP21 → INMP441 SD
    {"Key-Up",     2, false},
    {"Key-Down",  18, false},
    // {"Key-Left",  16, false},   // GP16 → INMP441 BCK
    // {"Key-Right", 20, false},   // GP20 → INMP441 LRCK
    {"Key-Ctrl",   3, false},
};
#define NUM_BUTTONS (sizeof(g_buttons)/sizeof(g_buttons[0]))

void buttons_init(void) {
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(g_buttons[i].pin);
        gpio_set_dir(g_buttons[i].pin, GPIO_IN);
        gpio_pull_up(g_buttons[i].pin);
    }
}

static void draw_button_row(int row, const char *name, bool pressed) {
    int y = ROW_Y0 + row * ROW_H;
    uint16_t bg = pressed ? COLOR_GREEN : COLOR_BLACK;
    uint16_t fg = pressed ? COLOR_BLACK : COLOR_WHITE;
    lcd_fill_rect(0, y, LCD_W, ROW_H - 2, bg);
    lcd_draw_text(TEXT_X, y + 3, name, FONT_SCALE, fg, bg);
    lcd_draw_text(IND_X,  y + 3, pressed ? "ON " : "OFF", FONT_SCALE, fg, bg);
}

void render_buttons_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "PicoLCD-1.3 Buttons", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        draw_button_row((int)i, g_buttons[i].name, g_buttons[i].pressed);
    }
}

//=============================================================================
// Lab views
//=============================================================================
void render_lab_loading(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Labs", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(8, ROW_Y0, "Fetching...", FONT_SCALE, COLOR_YELLOW, COLOR_BLACK);
}

void render_lab_error(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "Labs", FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(8, ROW_Y0, "Fetch error", FONT_SCALE, COLOR_RED, COLOR_BLACK);
}

void render_lab_list(void) {
    lcd_fill(COLOR_BLACK);
    bool any_active = g_timeline_active || g_linephone_active;
    const char *title = any_active
        ? "Labs (Enter=stop)"
        : "Labs (Enter=start)";
    lcd_draw_text(8, 4, title, FONT_SCALE, COLOR_CYAN, COLOR_BLACK);
    if (g_lab_count == 0) {
        lcd_draw_text(8, ROW_Y0, "(no labs)", FONT_SCALE, COLOR_WHITE, COLOR_BLACK);
        return;
    }
    for (int i = 0; i < g_lab_count; i++) {
        int y = ROW_Y0 + i * ROW_H;
        bool sel = (i == g_lab_selected);
        uint16_t bg = sel
            ? (any_active ? COLOR_YELLOW : COLOR_GREEN)
            : COLOR_BLACK;
        uint16_t fg = sel ? COLOR_BLACK : COLOR_WHITE;
        lcd_fill_rect(0, y, LCD_W, ROW_H - 2, bg);
        lcd_draw_text(TEXT_X, y + 3, g_labs[i].lab_id, FONT_SCALE, fg, bg);
    }
}

void render_lab_spinner(uint32_t slow_phase, bool fast_blink_on) {
    if (g_view != VIEW_LABS) return;
    const char *spinner_frames[] = {"|", "/", "-", "\\"};
    int sx = LCD_W - 14;
    int ix = LCD_W - 28;

    lcd_fill_rect(sx, 4, 12, 16, COLOR_BLACK);
    if (g_timeline_active) {
        const char *frame = spinner_frames[slow_phase & 3];
        lcd_draw_text(sx, 4, frame, FONT_SCALE, COLOR_YELLOW, COLOR_BLACK);
    }

    lcd_fill_rect(ix, 4, 12, 16, COLOR_BLACK);
    if (g_tts_play_active && fast_blink_on) {
        lcd_draw_text(ix, 4, "*", FONT_SCALE, COLOR_GREEN, COLOR_BLACK);
    }
}

//=============================================================================
// Fetch trigger
//=============================================================================
void trigger_fetch(void) {
    if (g_lab_state == LAB_LOADING) return;
    g_lab_state = LAB_LOADING;
    g_view      = VIEW_LABS;
    render_lab_loading();
    ic_send(IC_MSG_BTN_X, NULL, 0);
}

//=============================================================================
// Button event dispatch
//=============================================================================
static void on_button_event(int idx, bool now_pressed) {
    // Button-B is push-to-talk: it needs BOTH press (start mic) and release
    // (stop + post-end). Every other button only acts on press.
    if (idx == GUI_BTN_B) {
        if (g_linephone_active) {
            // Push-to-talk inside linephone mode: pause receive while talking
            // so the mic capture isn't drowned out by the speaker. The ring +
            // upstream pipeline are preserved — audio_resume() on release
            // picks up where DMA left off (catch-up artifact: up to a ring's
            // worth of buffered audio replays before catching live).
            if (now_pressed) {
                audio_pause();
                mic_start();
            } else {
                mic_stop();
                ic_send(IC_MSG_LINEPHONE_POST_END, NULL, 0);
                audio_resume();
            }
        } else if (now_pressed) {
            // Legacy / backward-compatible: outside linephone mode B just
            // stops in-flight audio output.
            audio_stop();
        }
        return;
    }

    if (!now_pressed) return;

    switch (idx) {
        case GUI_BTN_A:
            audio_play_sine();
            break;
        case GUI_BTN_X:
            trigger_fetch();
            break;
        // case GUI_BTN_Y:   // GP21 → INMP441 SD (no longer a button)
        //     g_view = VIEW_BUTTONS;
        //     render_buttons_view();
        //     break;
        case GUI_KEY_UP:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0
                && !g_timeline_active && !g_linephone_active
                && !g_tts_play_active) {
                g_lab_selected = (g_lab_selected - 1 + g_lab_count) % g_lab_count;
                render_lab_list();
            }
            break;
        case GUI_KEY_DOWN:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0
                && !g_timeline_active && !g_linephone_active
                && !g_tts_play_active) {
                g_lab_selected = (g_lab_selected + 1) % g_lab_count;
                render_lab_list();
            }
            break;
        case GUI_KEY_CTRL:
            if (g_view == VIEW_LABS && g_lab_state == LAB_OK && g_lab_count > 0) {
                const lab_entry_t *sel = &g_labs[g_lab_selected];
                if (sel->is_radio) {
                    // Re-press while a radio stream is playing → stop it. We
                    // use g_tts_play_active as the "stream live" signal
                    // because tts_start_playback flips it on once headers
                    // arrive, and the post-cleanup drain flips it back off.
                    if (g_tts_play_active) {
                        ic_send(IC_MSG_RADIO_STOP, NULL, 0);
                        printf("[core0] Enter: stop radio\n");
                    } else {
                        // Ship the radio operator's id with the start
                        // signal so core1 can pin the request body to
                        // it. Without this the server's default routing
                        // hijacks the response into a gameserver TTS
                        // when one is registered for this device.
                        ic_send(IC_MSG_RADIO_START,
                                sel->operator_id,
                                (uint16_t)strlen(sel->operator_id));
                        printf("[core0] Enter: start radio op=%s\n",
                               sel->operator_id);
                    }
                } else if (sel->is_linephone) {
                    // Toggle linephone mode. While active, core1 polls
                    // GET /api/device/linephone/?start=N and Button-B becomes
                    // push-to-talk (POST /api/device/linephone/).
                    if (g_linephone_active) {
                        // Defensive cleanup: if the user is still holding
                        // B when they stop linephone, mic is in capture
                        // state. Stop it and resume audio so we don't end
                        // up with a stuck mic + paused DAC. We deliberately
                        // do NOT ship IC_MSG_LINEPHONE_POST_END here — the
                        // session is going away, so the captured fragment
                        // should just be discarded.
                        if (mic_is_active()) {
                            mic_stop();
                            audio_resume();
                        }
                        ic_send(IC_MSG_LINEPHONE_STOP, NULL, 0);
                        g_linephone_active = false;
                        g_linephone_lab_id[0] = '\0';
                        printf("[core0] Enter: stop linephone\n");
                    } else {
                        strncpy(g_linephone_lab_id, sel->lab_id,
                                MAX_LAB_ID_LEN - 1);
                        g_linephone_lab_id[MAX_LAB_ID_LEN - 1] = '\0';
                        ic_send(IC_MSG_LINEPHONE_START,
                                sel->operator_id,
                                (uint16_t)strlen(sel->operator_id));
                        g_linephone_active = true;
                        printf("[core0] Enter: start linephone op=%s lab=%s\n",
                               sel->operator_id, g_linephone_lab_id);
                    }
                } else if (!g_timeline_active) {
                    strncpy(g_timeline_lab_id, sel->lab_id, MAX_LAB_ID_LEN - 1);
                    g_timeline_lab_id[MAX_LAB_ID_LEN - 1] = '\0';
                    const char *op = sel->operator_id;
                    ic_send(IC_MSG_TIMELINE_START, op, (uint16_t)strlen(op));
                    g_timeline_active = true;
                    printf("[core0] Enter: start timeline for lab=%s\n", g_timeline_lab_id);
                } else {
                    ic_send(IC_MSG_TIMELINE_STOP, NULL, 0);
                    g_timeline_active = false;
                    printf("[core0] Enter: stop timeline\n");
                }
                render_lab_list();
            }
            break;
        default:
            break;
    }
}

void buttons_poll(void) {
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        bool now_pressed = !gpio_get(g_buttons[i].pin);
        if (now_pressed != g_buttons[i].pressed) {
            g_buttons[i].pressed = now_pressed;
            printf("[core0] %-9s %s @ %lu ms\n",
                   g_buttons[i].name,
                   now_pressed ? "PRESS  " : "RELEASE",
                   (unsigned long)board_millis());

            if (g_view == VIEW_BUTTONS) {
                draw_button_row((int)i, g_buttons[i].name, now_pressed);
            }
            on_button_event((int)i, now_pressed);
        }
    }
}
