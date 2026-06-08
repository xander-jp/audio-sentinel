// WiFi credential persistence in the last sector of on-board flash.
//
// Layout in flash (little-endian, packed):
//   uint32_t magic    = CREDS_MAGIC
//   uint32_t version  = CREDS_VERSION
//   uint8_t  ssid_len            (0..32, no trailing NUL counted)
//   uint8_t  pass_len            (0..64, no trailing NUL counted)
//   uint8_t  device_id_len       (0..64, no trailing NUL counted)
//   uint8_t  _pad[1]
//   char     ssid[CREDS_SSID_MAX]            (zero-padded)
//   char     password[CREDS_PASS_MAX]        (zero-padded)
//   char     device_id[CREDS_DEVICE_ID_MAX]  (zero-padded)
//   uint32_t crc32   (over all preceding bytes)
//
// Flash writes go through pico-sdk's flash_safe_execute() which suspends the
// other core's XIP access, so callers don't need their own locking. WiFi/BLE
// must be idle when creds_save() runs (we only call it during the
// provisioning bootmode, before launching core1).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CREDS_SSID_MAX        32
#define CREDS_PASS_MAX        64
// Server-issued device UUID. RFC4122 form is 36 chars; we allow up to 64 to
// leave room for longer formats (URN prefix, base64, etc.) without another
// schema bump.
#define CREDS_DEVICE_ID_MAX   64

// Despite the "wifi_creds" name, this also stores the server-side device ID
// alongside the AP credentials — they're provisioned in the same BLE flow
// and consumed together (WiFi to connect, device_id to authenticate API
// calls).
typedef struct {
    char    ssid[CREDS_SSID_MAX + 1];           // +1 for guaranteed NUL terminator
    char    password[CREDS_PASS_MAX + 1];
    char    device_id[CREDS_DEVICE_ID_MAX + 1];
    bool    opus_enabled;                       // true → request opus codec on TTS
} wifi_creds_t;

// Returns 0 if a valid, CRC-verified set of creds was loaded into *out.
// Returns -1 if flash is blank, magic/version mismatch, or CRC fails.
int  creds_load(wifi_creds_t *out);

// Persist creds to flash. Returns 0 on success, -1 on error.
// Caller must guarantee no other core is currently using flash/lwIP.
int  creds_save(const wifi_creds_t *in);

// Wipe the credential sector (sets it to all-0xFF).
int  creds_erase(void);
