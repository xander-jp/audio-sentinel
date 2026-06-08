// WiFi initialization and DNS resolution.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "lwip/ip_addr.h"

// set_status is declared in gui.h — include it for wifi.c to call.
#include "gui.h"
#include "credentials.h"

// Connect to the AP whose SSID/PSK are passed in (blocking, 30 s timeout).
// Credentials come from flash (loaded via creds_load) at boot — see main.c.
int wifi_init(const char *ssid, const char *password);

// Extract hostname from a URL (e.g. "https://host:port/path" -> "host").
void extract_hostname(const char *url, char *hostname, size_t hostname_size);

// Resolve `hostname` via lwIP DNS (blocking, 10 s timeout).
int dns_resolve_blocking(const char *hostname, ip_addr_t *out);

// Resolved API server address (set by network_init).
extern ip_addr_t g_api_ip;

// wifi_init + DNS resolve + populate g_api_ip / g_https_host.
// `creds` must contain a SSID (and password if the AP is WPA2-PSK).
int network_init(const wifi_creds_t *creds);
