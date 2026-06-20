// LCD GUI: status log, button handling, and lab-list rendering.
#pragma once

#include <stdbool.h>
#include <stdint.h>

//-----------------------------------------------------------------------------
// Button IDs (indices into the internal g_buttons[] table)
//-----------------------------------------------------------------------------
typedef enum {
    // GP16 / GP20 / GP21 (formerly Key-Left / Key-Right / Button-Y on the
    // Waveshare LCD) are now wired to the INMP441 I2S RX (BCK/LRCK/SD), so
    // those three are no longer polled as buttons — entries are commented
    // out (in both this enum and g_buttons[]) so the indexing stays aligned.
    GUI_BTN_A = 0,
    GUI_BTN_B,
    GUI_BTN_X,
    // GUI_BTN_Y,       // GP21 → INMP441 SD
    GUI_KEY_UP,
    GUI_KEY_DOWN,
    // GUI_KEY_LEFT,    // GP16 → INMP441 BCK
    // GUI_KEY_RIGHT,   // GP20 → INMP441 LRCK
    GUI_KEY_CTRL,
    GUI_BTN_COUNT,
} gui_button_id_t;

//-----------------------------------------------------------------------------
// View state
//-----------------------------------------------------------------------------
typedef enum {
    VIEW_STATUS,
    VIEW_BUTTONS,
    VIEW_LABS,
} view_state_t;

//-----------------------------------------------------------------------------
// Lab list state
//-----------------------------------------------------------------------------
typedef enum {
    LAB_IDLE,
    LAB_LOADING,
    LAB_OK,
    LAB_ERR,
} lab_state_t;

#define MAX_LABS        16
#define MAX_LAB_ID_LEN  64

// Shared state — defined in gui.c, read/written from main.c core loops.
extern volatile view_state_t g_view;
extern volatile lab_state_t  g_lab_state;
// One row per operator/lab pair surfaced by /api/tunnel/info. Roles
// drive Enter-key behavior on the lab list (radio = streaming pcb path,
// gameserver = (future) auto-target path, else = manual timeline).
typedef struct lab_entry {
    char operator_id[MAX_LAB_ID_LEN];
    char lab_id[MAX_LAB_ID_LEN];
    bool is_radio;
    bool is_gameserver;
    bool is_linephone;
} lab_entry_t, *lab_entry_ptr;

extern lab_entry_t g_labs[MAX_LABS];
extern int         g_lab_count;
extern int         g_lab_selected;
extern bool     g_timeline_active;
extern char     g_timeline_lab_id[MAX_LAB_ID_LEN];
extern volatile int  g_timeline_status_ver;
extern volatile bool g_tts_play_active;

// Linephone (walkie-talkie) mode flag. Set when the user presses Enter on
// a role=linephone lab; gates the GET polling on core1 and the mic-on-B
// behavior on core0. Mutually exclusive with timeline/radio.
extern bool          g_linephone_active;
extern char          g_linephone_lab_id[MAX_LAB_ID_LEN];

//-----------------------------------------------------------------------------
// Status log (thread-safe, called from both cores via wifi.c / main.c)
//-----------------------------------------------------------------------------
void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Returns the monotonic version counter; caller compares to detect updates.
uint32_t gui_status_version(void);

//-----------------------------------------------------------------------------
// LCD layout constants
//-----------------------------------------------------------------------------
#define FONT_SCALE  2
#define ROW_H       22
#define ROW_Y0      28
#define TEXT_X      8
#define IND_X       160

//-----------------------------------------------------------------------------
// Functions called from core0_loop
//-----------------------------------------------------------------------------
void buttons_init(void);
void buttons_poll(void);

// Button-B (push-to-talk) action, factored out of the button dispatcher so the
// sonar proximity trigger in main.c can drive the exact same mic ON/OFF path.
// now_pressed=true → press (mic start), false → release (mic stop + POST_END).
void gui_button_b(bool now_pressed);

// Stop push-to-talk explicitly, choosing whether to upload. do_post=false
// drops the capture on core1 (used by the sonar trigger when the talk was
// shorter than the minimum worth posting). No-op outside linephone mode.
void gui_ptt_stop(bool do_post);

void render_status_view(void);
void render_buttons_view(void);
void render_lab_loading(void);
void render_lab_error(void);
void render_lab_list(void);
void render_lab_spinner(uint32_t slow_phase, bool fast_blink_on);

// Trigger a lab-list fetch (sends IC_MSG_BTN_X to core1).
void trigger_fetch(void);

// Run the Enter-key action on the currently selected lab row. Called both
// from the button dispatcher (GUI_KEY_CTRL) and from the single-result
// auto-pilot path in the IC_MSG_LABS_READY handler.
void lab_enter_action(void);
