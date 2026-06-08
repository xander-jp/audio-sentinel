#include "ble_provision.h"
#include "common.h"
#include "gui.h"
#include "st7789.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "btstack.h"

// Generated from src/wifi_provision.gatt by pico_btstack_make_gatt_header().
// Defines `profile_data[]` and ATT_CHARACTERISTIC_*_VALUE_HANDLE macros.
#include "wifi_provision.h"

//=============================================================================
// Provisioning state machine — flips to PROV_COMMITTED when the phone writes
// 0x01 to the Commit characteristic, which unblocks ble_provision_run().
//=============================================================================
typedef enum {
    PROV_RUNNING = 0,
    PROV_COMMITTED,
    PROV_ABORTED,
} prov_state_t;

// Status byte exposed via the Status characteristic + Notify.
typedef enum {
    PROV_STATUS_IDLE     = 0,
    PROV_STATUS_SSID_OK  = 1,
    PROV_STATUS_PASS_OK  = 2,
    PROV_STATUS_DEVID_OK = 3,
    PROV_STATUS_SAVED    = 4,
    PROV_STATUS_OPUS_OK  = 5,
    PROV_STATUS_ERR      = 0xFF,
} prov_status_t;

static volatile prov_state_t   g_prov_state  = PROV_RUNNING;
static volatile prov_status_t  g_prov_status = PROV_STATUS_IDLE;

// Accumulated writes — zeroed on entry, fields populated as the phone
// writes each characteristic. Because copy_value() always NUL-terminates
// and we reject empty SSID/DeviceID writes at the ATT layer, a non-zero
// first byte is sufficient to mean "this field has been written".
static wifi_creds_t            g_pending;
static bool                    g_connected  = false;

static hci_con_handle_t        g_con_handle      = HCI_CON_HANDLE_INVALID;
static uint16_t                g_status_cccd     = 0;   // notify-enable flag

// Bumped whenever any state on the provisioning screen needs a redraw.
static volatile uint32_t       g_prov_ui_version = 0;
static inline void prov_ui_dirty(void) { g_prov_ui_version++; }

//=============================================================================
// Provisioning view — dedicated full-screen LCD layout shown while we wait
// for the phone to finish writing SSID/Password/Commit. Re-rendered every
// time the state advances.
//=============================================================================
static void render_prov_view(void) {
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(8, 4, "BLE WiFi Setup", 2, COLOR_CYAN, COLOR_BLACK);

    // Device name banner — what the phone scanner will see.
    lcd_draw_text(8, 32, "Name:", 2, COLOR_GRAY, COLOR_BLACK);
    lcd_draw_text(72, 32, "Ito-Denwa Prov", 2, COLOR_YELLOW, COLOR_BLACK);

    // Connection state
    int y = 64;
    uint16_t cc = g_connected ? COLOR_GREEN : COLOR_GRAY;
    lcd_draw_text(8, y, g_connected ? "[*] Linked" : "[ ] Awaiting BLE",
                  2, cc, COLOR_BLACK);

    // Step checklist — a non-empty first byte means the phone wrote that
    // characteristic this session. Note: an open AP with no password will
    // legitimately stay "[ ] Password set" (we never write the field), and
    // that's fine — the commit gate only requires SSID + Device ID.
    bool has_ssid  = g_pending.ssid[0]      != '\0';
    bool has_pass  = g_pending.password[0]  != '\0';
    bool has_devid = g_pending.device_id[0] != '\0';
    y += 28;
    lcd_draw_text(8, y, has_ssid ? "[*] SSID set" : "[ ] SSID set",
                  2, has_ssid ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);
    y += 22;
    lcd_draw_text(8, y, has_pass ? "[*] Password set" : "[ ] Password set",
                  2, has_pass ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);
    y += 22;
    lcd_draw_text(8, y, has_devid ? "[*] Device ID set" : "[ ] Device ID set",
                  2, has_devid ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);
    y += 22;
    lcd_draw_text(8, y, g_pending.opus_enabled ? "[*] Opus: ON" : "[ ] Opus: OFF",
                  2, g_pending.opus_enabled ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);
    y += 22;
    bool committed = (g_prov_state == PROV_COMMITTED);
    lcd_draw_text(8, y, committed ? "[*] Committed" : "[ ] Commit",
                  2, committed ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);

    // Footer hint
    lcd_draw_text(8, LCD_H - 22, "Hold Y at boot to reset",
                  2, COLOR_DARKGRAY, COLOR_BLACK);
}

//=============================================================================
// Advertising / scan-response data
//
// BLE legacy advertising payload is capped at 31 bytes. Flags (3) + 128-bit
// service UUID (18) + the 14-char name (16) totals 37, which silently fails
// the HCI command. So we keep the UUID in adv_data (what iOS uses for
// service-UUID filtering) and move the human-readable name into the
// scan-response — phones doing active scans get the name automatically.
//
// 128-bit UUID has to be byte-reversed for the AD payload:
//   A2D40000-2D11-1AE1-2D80-1A30CCAE0001
// → 01 00 AE CC 30 1A 80 2D E1 1A 11 2D 00 00 D4 A2
//=============================================================================
static const uint8_t adv_data[] = {
    // Flags: LE general discoverable, BR/EDR not supported   (3 bytes)
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    // Complete list of 128-bit service UUIDs (1×16 bytes)    (18 bytes)
    0x11, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
        0x01, 0x00, 0xAE, 0xCC, 0x30, 0x1A, 0x80, 0x2D,
        0xE1, 0x1A, 0x11, 0x2D, 0x00, 0x00, 0xD4, 0xA2,
};                                                            // total 21 bytes
static const uint8_t adv_data_len = sizeof(adv_data);

static const uint8_t scan_response_data[] = {
    // Complete local name: "Ito-Denwa Prov"                  (16 bytes)
    0x0F, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
        'I','t','o','-','D','e','n','w','a',' ','P','r','o','v',
};                                                            // total 16 bytes
static const uint8_t scan_response_data_len = sizeof(scan_response_data);

//=============================================================================
// ATT handles (from generated wifi_provision.h)
//
// compile_gatt.py emits a macro per characteristic; we alias them here so the
// callbacks read cleanly.
//=============================================================================
#define H_SSID    ATT_CHARACTERISTIC_A2D40001_2D11_1AE1_2D80_1A30CCAE0001_01_VALUE_HANDLE
#define H_PASS    ATT_CHARACTERISTIC_A2D40002_2D11_1AE1_2D80_1A30CCAE0001_01_VALUE_HANDLE
#define H_COMMIT  ATT_CHARACTERISTIC_A2D40003_2D11_1AE1_2D80_1A30CCAE0001_01_VALUE_HANDLE
#define H_STATUS  ATT_CHARACTERISTIC_A2D40004_2D11_1AE1_2D80_1A30CCAE0001_01_VALUE_HANDLE
#define H_STATUS_CCCD \
    ATT_CHARACTERISTIC_A2D40004_2D11_1AE1_2D80_1A30CCAE0001_01_CLIENT_CONFIGURATION_HANDLE
#define H_DEVID   ATT_CHARACTERISTIC_A2D40005_2D11_1AE1_2D80_1A30CCAE0001_01_VALUE_HANDLE
#define H_OPUS    ATT_CHARACTERISTIC_A2D40006_2D11_1AE1_2D80_1A30CCAE0001_01_VALUE_HANDLE

//=============================================================================
// Notification helper.
//
// att_server_register_can_send_now_callback() appends the registration
// struct to a btstack-internal linked list — appending the SAME struct
// twice corrupts the list (its `next` pointer is overwritten) and from
// then on no notifications ever fire. iOS retrying a .withoutResponse
// write triggers this easily, so we gate re-registration on a pending
// flag and just coalesce: the latest g_prov_status will be sent when the
// already-registered callback fires.
//=============================================================================
static btstack_context_callback_registration_t g_status_send_cb;
static volatile bool                            g_notify_pending = false;

static void status_notify_can_send_now(void *ctx) {
    (void)ctx;
    g_notify_pending = false;
    if (g_con_handle == HCI_CON_HANDLE_INVALID) return;
    uint8_t v = (uint8_t)g_prov_status;
    uint8_t rc = att_server_notify(g_con_handle, H_STATUS, &v, 1);
    printf("[ble] notify status=%u rc=%u\n", v, rc);
}

static void notify_status(prov_status_t s) {
    g_prov_status = s;
    if (g_con_handle == HCI_CON_HANDLE_INVALID) {
        printf("[ble] notify_status(%u) skipped: not connected\n", s);
        return;
    }
    if (!g_status_cccd) {
        // iOS hasn't called setNotifyValue(true, ...) yet — CCCD write
        // either hasn't happened or hasn't been processed. Notifications
        // are silently dropped per BLE spec; iOS apps must subscribe
        // before issuing the writes whose ACKs they expect.
        printf("[ble] notify_status(%u) skipped: CCCD not subscribed\n", s);
        return;
    }
    if (g_notify_pending) {
        printf("[ble] notify_status(%u) coalesced (prev still pending)\n", s);
        return;
    }
    g_notify_pending          = true;
    g_status_send_cb.callback = &status_notify_can_send_now;
    g_status_send_cb.context  = NULL;
    att_server_register_can_send_now_callback(&g_status_send_cb, g_con_handle);
    printf("[ble] notify_status(%u) registered can_send_now\n", s);
}

//=============================================================================
// ATT read callback — only the Status characteristic is readable.
//=============================================================================
static uint16_t att_read_cb(hci_con_handle_t con, uint16_t att_handle,
                            uint16_t offset, uint8_t *buffer,
                            uint16_t buffer_size) {
    (void)con;
    if (att_handle == H_STATUS) {
        uint8_t v = (uint8_t)g_prov_status;
        return att_read_callback_handle_blob(&v, 1, offset, buffer, buffer_size);
    }
    return 0;
}

//=============================================================================
// ATT write callback — collect SSID / Password / Commit.
//=============================================================================
static void copy_value(char *dst, size_t dst_cap, const uint8_t *src, uint16_t len) {
    if (len >= dst_cap) len = (uint16_t)(dst_cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int att_write_cb(hci_con_handle_t con, uint16_t att_handle,
                        uint16_t transaction_mode, uint16_t offset,
                        uint8_t *buffer, uint16_t buffer_size) {
    (void)con; (void)offset;
    if (transaction_mode != ATT_TRANSACTION_MODE_NONE) {
        // We don't support long writes — reject prepared writes.
        return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    }

    if (att_handle == H_SSID) {
        if (buffer_size == 0 || buffer_size > CREDS_SSID_MAX)
            return ATT_ERROR_VALUE_NOT_ALLOWED;
        copy_value(g_pending.ssid, sizeof(g_pending.ssid), buffer, buffer_size);
        printf("[ble] SSID written (%u bytes) '%s'\n", buffer_size, g_pending.ssid);
        set_status("BLE: SSID set");
        notify_status(PROV_STATUS_SSID_OK);
        prov_ui_dirty();
        return 0;
    }
    if (att_handle == H_PASS) {
        if (buffer_size > CREDS_PASS_MAX)
            return ATT_ERROR_VALUE_NOT_ALLOWED;
        copy_value(g_pending.password, sizeof(g_pending.password), buffer, buffer_size);
        printf("[ble] Password written (%u bytes)\n", buffer_size);
        set_status("BLE: pass set");
        notify_status(PROV_STATUS_PASS_OK);
        prov_ui_dirty();
        return 0;
    }
    if (att_handle == H_DEVID) {
        if (buffer_size == 0 || buffer_size > CREDS_DEVICE_ID_MAX)
            return ATT_ERROR_VALUE_NOT_ALLOWED;
        copy_value(g_pending.device_id, sizeof(g_pending.device_id),
                   buffer, buffer_size);
        printf("[ble] DeviceID written (%u bytes) '%s'\n",
               buffer_size, g_pending.device_id);
        set_status("BLE: devID set");
        notify_status(PROV_STATUS_DEVID_OK);
        prov_ui_dirty();
        return 0;
    }
    if (att_handle == H_OPUS) {
        if (buffer_size < 1) return ATT_ERROR_VALUE_NOT_ALLOWED;
        g_pending.opus_enabled = (buffer[0] != 0);
        printf("[ble] Opus written: %d\n", (int)g_pending.opus_enabled);
        notify_status(PROV_STATUS_OPUS_OK);
        prov_ui_dirty();
        return 0;
    }
    if (att_handle == H_COMMIT) {
        if (buffer_size < 1) return ATT_ERROR_VALUE_NOT_ALLOWED;
        if (buffer[0] != 0x01) return 0;  // only 0x01 triggers commit
        if (g_pending.ssid[0] == '\0' || g_pending.device_id[0] == '\0') {
            printf("[ble] commit rejected: ssid='%s' devid='%s'\n",
                   g_pending.ssid, g_pending.device_id);
            notify_status(PROV_STATUS_ERR);
            return ATT_ERROR_WRITE_REQUEST_REJECTED;
        }
        printf("[ble] commit received\n");
        set_status("BLE: commit");
        notify_status(PROV_STATUS_SAVED);
        g_prov_state = PROV_COMMITTED;
        prov_ui_dirty();
        return 0;
    }
    if (att_handle == H_STATUS_CCCD) {
        g_status_cccd = little_endian_read_16(buffer, 0);
        printf("[ble] status CCCD=0x%04x\n", g_status_cccd);
        return 0;
    }
    return 0;
}

//=============================================================================
// HCI / ATT event handler — track connection state.
//=============================================================================
static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE: {
        // Fires every time the controller transitions. We only really care
        // that we reach HCI_STATE_WORKING — at which point advertising will
        // actually start on air. If we never see this, CYW43 BT firmware
        // probably wasn't loaded (check CYW43_ENABLE_BLUETOOTH=1).
        uint8_t s = btstack_event_state_get_state(packet);
        printf("[ble] BTSTACK_EVENT_STATE = %u%s\n", s,
               s == HCI_STATE_WORKING ? " (WORKING — advertising live)" : "");
        if (s == HCI_STATE_WORKING) {
            set_status("BLE: adv on air");
        }
        break;
    }
    case HCI_EVENT_LE_META:
        if (hci_event_le_meta_get_subevent_code(packet)
                == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            g_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            g_connected  = true;
            printf("[ble] connected, handle=0x%04x\n", g_con_handle);
            set_status("BLE: connected");
            prov_ui_dirty();
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("[ble] disconnected\n");
        g_con_handle     = HCI_CON_HANDLE_INVALID;
        g_status_cccd    = 0;
        g_connected      = false;
        g_notify_pending = false;
        if (g_prov_state == PROV_RUNNING) {
            set_status("BLE: waiting...");
            // Re-enable advertising so a fresh client can connect.
            gap_advertisements_enable(1);
        }
        prov_ui_dirty();
        break;
    default:
        break;
    }
}

//=============================================================================
// Public entry — bring up the stack, advertise, wait for Commit.
//=============================================================================
int ble_provision_run(wifi_creds_t *out, uint32_t timeout_ms) {
    if (!out) return -1;

    memset(&g_pending, 0, sizeof(g_pending));
    // Default Opus to OFF so the device stays on the known-working raw-PCM
    // path unless the phone explicitly opts in. Phones that don't yet know
    // about the v3 schema will leave this at 0 and keep working.
    g_pending.opus_enabled = false;
    g_connected   = false;
    g_prov_state  = PROV_RUNNING;
    g_prov_status = PROV_STATUS_IDLE;
    g_con_handle  = HCI_CON_HANDLE_INVALID;
    g_status_cccd = 0;

    // L2CAP + SM + ATT
    l2cap_init();
    sm_init();
    att_server_init(profile_data, att_read_cb, att_write_cb);

    // Advertising parameters: 30 ms interval (0x30 * 0.625 ms ≈ 30 ms)
    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    uint8_t  adv_type    = 0;  // ADV_IND
    bd_addr_t null_addr = {0};
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type,
                                  0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(adv_data_len, (uint8_t *)adv_data);
    gap_scan_response_set_data(scan_response_data_len,
                               (uint8_t *)scan_response_data);
    gap_advertisements_enable(1);

    // HCI + ATT event registrations
    static btstack_packet_callback_registration_t hci_cb_reg;
    hci_cb_reg.callback = &packet_handler;
    hci_add_event_handler(&hci_cb_reg);
    att_server_register_packet_handler(packet_handler);

    // Power up the BT controller
    hci_power_control(HCI_POWER_ON);

    printf("[ble] provisioning advertise started\n");
    set_status("BLE: waiting...");

    // First paint of the provisioning view (subsequent paints happen below
    // whenever g_prov_ui_version changes — which is whenever the GATT write
    // callback or HCI connection handler calls prov_ui_dirty()).
    render_prov_view();
    uint32_t last_ui_version = g_prov_ui_version;

    uint32_t t0 = board_millis();
    while (g_prov_state == PROV_RUNNING) {
        if (timeout_ms && (board_millis() - t0) > timeout_ms) {
            printf("[ble] timeout waiting for commit\n");
            g_prov_state = PROV_ABORTED;
            break;
        }
        if (g_prov_ui_version != last_ui_version) {
            last_ui_version = g_prov_ui_version;
            render_prov_view();
        }
        // CYW43 + btstack pump via the threadsafe-background async_context.
        // Just yield CPU; the BT/HCI work happens in IRQ + alarm contexts.
        sleep_ms(50);
    }

    // Final paint reflects the COMMITTED/ABORTED state before we tear down.
    render_prov_view();

    // Give the notify of PROV_STATUS_SAVED a chance to land before we tear
    // down the link.
    sleep_ms(200);

    gap_advertisements_enable(0);
    hci_power_control(HCI_POWER_OFF);

    if (g_prov_state != PROV_COMMITTED) return -1;

    memcpy(out, &g_pending, sizeof(*out));
    return 0;
}
