#include "led.h"
#include "common.h"

#include "pico/cyw43_arch.h"

//=============================================================================
// LED blink state machine (non-blocking)
//=============================================================================
typedef struct {
    uint32_t interval_ms;
    int      remaining;
    uint32_t last_toggle;
    bool     led_on;
} LedBlinkState;

static LedBlinkState g_led_blink = {0, 0, 0, false};

void led_blink_start(uint32_t interval_ms, int count) {
    g_led_blink.interval_ms = interval_ms;
    g_led_blink.remaining   = count * 2;
    g_led_blink.last_toggle = board_millis();
    g_led_blink.led_on      = true;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
}

void led_blink_loop(void) {
    if (g_led_blink.remaining <= 0) return;

    uint32_t now = board_millis();
    if ((now - g_led_blink.last_toggle) >= g_led_blink.interval_ms) {
        g_led_blink.last_toggle = now;
        g_led_blink.remaining--;

        if (g_led_blink.remaining > 0) {
            g_led_blink.led_on = !g_led_blink.led_on;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, g_led_blink.led_on ? 1 : 0);
        } else {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            g_led_blink.led_on = false;
        }
    }
}

static uint32_t g_heartbeat_last = 0;
static bool     g_heartbeat_on   = false;

void led_heartbeat_loop(void) {
    if (g_led_blink.remaining > 0) return;

    uint32_t now = board_millis();
    if ((now - g_heartbeat_last) >= 1000) {
        g_heartbeat_last = now;
        g_heartbeat_on   = !g_heartbeat_on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, g_heartbeat_on ? 1 : 0);
    }
}
