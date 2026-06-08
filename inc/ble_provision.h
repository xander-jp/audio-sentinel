// BLE WiFi-provisioning service (peripheral).
//
// Brings up the CYW43 BLE controller, registers the GATT database compiled
// from `wifi_provision.gatt`, and waits for a phone-side configurator to:
//   1. Write the SSID characteristic       (UTF-8, up to 32 bytes)
//   2. Write the Password characteristic   (UTF-8, up to 64 bytes)
//   3. Write the Commit characteristic with 0x01
// On Commit the SSID/PSK pair is returned via *out and the function returns.
// Caller is expected to persist creds (creds_save) then reboot.
//
// While advertising, the device shows progress on the LCD via set_status().
#pragma once

#include "credentials.h"

// Block until the user writes Commit (or `timeout_ms` elapses, 0 = no timeout).
// Caller must have run cyw43_arch_init() first.
// Returns 0 on success (*out populated), -1 on timeout / abort.
int ble_provision_run(wifi_creds_t *out, uint32_t timeout_ms);
