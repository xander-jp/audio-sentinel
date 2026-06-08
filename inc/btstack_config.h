// btstack configuration for BLE peripheral (WiFi provisioning service).
//
// Pared down to the minimum needed for an LE peripheral with one custom
// 128-bit GATT service; no Classic, no LE Central, no bonding/SM secure
// connections — the device is intended to be re-provisioned only via
// physical button hold, so we accept "Just Works" pairing-less writes.
#pragma once

// === Features =============================================================
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_DATA_CHANNELS
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// === Buffer / connection counts ===========================================
// One peripheral connection is all we need for the provisioning flow.
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE        (255 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#define MAX_NR_HCI_CONNECTIONS       1
#define MAX_NR_L2CAP_CHANNELS        4
#define MAX_NR_L2CAP_SERVICES        2
#define MAX_NR_GATT_CLIENTS          0
#define MAX_NR_SM_LOOKUP_ENTRIES     2
#define MAX_NR_WHITELIST_ENTRIES     1
#define MAX_NR_LE_DEVICE_DB_ENTRIES  1

// Match Pico SDK kitchen_sink to avoid cyw43 shared-bus overruns.
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN     1024
#define HCI_HOST_ACL_PACKET_NUM     3
#define HCI_HOST_SCO_PACKET_LEN     120
#define HCI_HOST_SCO_PACKET_NUM     3

// ATT DB is statically sized — we control the .gatt content so 512 B is plenty.
#define MAX_ATT_DB_SIZE             512

// === HAL ==================================================================
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

// SM / crypto — required by btstack core even when bonding is unused.
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

// NVM not needed (no bonding) but provide stubs anyway.
#define NVM_NUM_DEVICE_DB_ENTRIES 1
#define NVM_NUM_LINK_KEYS         1
