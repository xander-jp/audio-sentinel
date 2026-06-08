#include "wifi.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "http.h"

#ifndef API_URL
#define API_URL "https://example.com/"
#endif

//=============================================================================
// WiFi initialization (detailed timing)
//=============================================================================
int wifi_init(const char *ssid, const char *password) {
    int last_status = CYW43_LINK_DOWN;
    int status;
    uint32_t now;
    uint32_t timeout_start;

    uint32_t time_wifi_start;
    uint32_t time_wifi_scan_start;
    uint32_t time_dhcp_start = 0;

    if (!ssid || !ssid[0]) {
        printf("[wifi] empty SSID — refusing to connect\n");
        set_status("WiFi: no SSID");
        return -1;
    }
    // Open networks: pass NULL password + AUTH_OPEN. Otherwise WPA2-PSK.
    bool open_ap = (password == NULL) || (password[0] == '\0');
    uint32_t auth = open_ap ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;

    cyw43_arch_enable_sta_mode();

    time_wifi_start      = board_millis();
    time_wifi_scan_start = time_wifi_start;
    printf("[TIMING] WiFi scan start: %lu ms\n", (unsigned long)time_wifi_scan_start);
    printf("Connecting to WiFi '%s'...\n", ssid);
    set_status("WiFi: '%s' scan...", ssid);

    if (cyw43_arch_wifi_connect_async(ssid, password, auth) != 0) {
        printf("WiFi connect_async failed\n");
        set_status("WiFi: connect_async fail");
        return -1;
    }

    timeout_start = board_millis();
    while (1) {
        cyw43_arch_poll();
        sleep_ms(10);

        now = board_millis();
        if ((now - timeout_start) > 30000) {
            printf("WiFi connection timeout\n");
            set_status("WiFi: timeout");
            return -1;
        }

        status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != last_status) {
            switch (status) {
                case CYW43_LINK_JOIN:
                    printf("[TIMING] WiFi scan done: %lu ms (took %lu ms)\n",
                           (unsigned long)now, (unsigned long)(now - time_wifi_scan_start));
                    printf("[TIMING] WiFi auth/assoc done: %lu ms\n", (unsigned long)now);
                    set_status("WiFi: assoc ok (%lums)", (unsigned long)(now - time_wifi_scan_start));
                    break;
                case CYW43_LINK_NOIP:
                    time_dhcp_start = now;
                    printf("[TIMING] DHCP start: %lu ms\n", (unsigned long)now);
                    set_status("DHCP: requesting...");
                    break;
                case CYW43_LINK_UP: {
                    printf("[TIMING] DHCP bound: %lu ms (took %lu ms)\n",
                           (unsigned long)now, (unsigned long)(now - time_dhcp_start));
                    printf("[TIMING] WiFi fully connected: %lu ms (total %lu ms)\n",
                           (unsigned long)now, (unsigned long)(now - time_wifi_start));
                    const ip_addr_t *my_ip = netif_ip_addr4(netif_default);
                    set_status("DHCP: %s", my_ip ? ipaddr_ntoa(my_ip) : "?");
                    set_status("WiFi: up (%lums)", (unsigned long)(now - time_wifi_start));
                    goto wifi_connected;
                }
                case CYW43_LINK_FAIL:
                    printf("WiFi connection failed\n");
                    set_status("WiFi: fail");
                    return -1;
                case CYW43_LINK_NONET:
                    printf("WiFi: No matching SSID found\n");
                    set_status("WiFi: no SSID");
                    return -1;
                case CYW43_LINK_BADAUTH:
                    printf("WiFi: Authentication failure\n");
                    set_status("WiFi: bad auth");
                    return -1;
                default:
                    break;
            }
            last_status = status;
        }
    }

wifi_connected:
    for (int i = 0; i < 5; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(100);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(100);
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    return 0;
}

//=============================================================================
// DNS resolution (blocking)
//=============================================================================
static volatile bool g_dns_done = false;
static ip_addr_t     g_dns_result;
static uint32_t      g_dns_start_ms;
static uint32_t      g_dns_done_ms;

static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name; (void)arg;
    g_dns_done_ms = board_millis();
    if (ipaddr) {
        g_dns_result = *ipaddr;
        printf("[TIMING] DNS resolved: %lu ms (took %lu ms) -> %s\n",
               (unsigned long)g_dns_done_ms,
               (unsigned long)(g_dns_done_ms - g_dns_start_ms),
               ipaddr_ntoa(ipaddr));
    } else {
        printf("[TIMING] DNS resolution failed: %lu ms\n", (unsigned long)g_dns_done_ms);
    }
    g_dns_done = true;
}

void extract_hostname(const char *url, char *hostname, size_t hostname_size) {
    const char *start = url;
    const char *end;
    if (strncmp(url, "http://", 7) == 0)  start = url + 7;
    else if (strncmp(url, "https://", 8) == 0) start = url + 8;
    end = start;
    while (*end && *end != ':' && *end != '/') end++;
    size_t len = end - start;
    if (len >= hostname_size) len = hostname_size - 1;
    memcpy(hostname, start, len);
    hostname[len] = '\0';
}

int dns_resolve_blocking(const char *hostname, ip_addr_t *out) {
    g_dns_start_ms = board_millis();
    printf("[TIMING] DNS lookup start: %lu ms (host: %s)\n",
           (unsigned long)g_dns_start_ms, hostname);
    set_status("DNS: %s", hostname);

    g_dns_done = false;
    err_t err = dns_gethostbyname(hostname, &g_dns_result, dns_callback, NULL);
    if (err == ERR_OK) {
        g_dns_done_ms = board_millis();
        printf("[TIMING] DNS resolved (cached): %lu ms -> %s\n",
               (unsigned long)g_dns_done_ms, ipaddr_ntoa(&g_dns_result));
        *out = g_dns_result;
        set_status("DNS: %s (cached)", ipaddr_ntoa(&g_dns_result));
        return 0;
    } else if (err == ERR_INPROGRESS) {
        uint32_t dns_timeout = board_millis();
        while (!g_dns_done && (board_millis() - dns_timeout) < 10000) {
            cyw43_arch_poll();
            sleep_ms(10);
        }
        if (!g_dns_done) {
            printf("[TIMING] DNS timeout\n");
            set_status("DNS: timeout");
            return -1;
        }
        *out = g_dns_result;
        set_status("DNS: %s", ipaddr_ntoa(&g_dns_result));
        return 0;
    } else {
        printf("[TIMING] DNS lookup failed: err=%d\n", err);
        set_status("DNS: err=%d", err);
        return -1;
    }
}

ip_addr_t g_api_ip;

int network_init(const wifi_creds_t *creds) {
    if (!creds) return -1;
    if (wifi_init(creds->ssid, creds->password) != 0) return -1;
    srand(board_millis());

    char hostname[128];
    extract_hostname(API_URL, hostname, sizeof(hostname));
    if (dns_resolve_blocking(hostname, &g_api_ip) != 0) {
        printf("[net] DNS failed for %s\n", hostname);
        return -1;
    }
    strncpy(g_https_host, hostname, sizeof(g_https_host) - 1);
    g_https_host[sizeof(g_https_host) - 1] = '\0';
    return 0;
}
