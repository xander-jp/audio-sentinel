#ifndef _LWIPOPTS_EXAMPLE_COMMONH_H
#define _LWIPOPTS_EXAMPLE_COMMONH_H

// Settings for the pico_w HTTPS client example
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

#ifndef NO_SYS
#define NO_SYS                      1
#endif
// Socket API requires NO_SYS=0. We use lwIP raw API + altcp_tls for HTTPS.
#ifndef LWIP_SOCKET
#define LWIP_SOCKET                 0
#endif
// Prevent redefinition of struct timeval (already defined in ARM toolchain)
#define LWIP_TIMEVAL_PRIVATE        0

#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC             1
#else
// MEM_LIBC_MALLOC is incompatible with non polling versions
#define MEM_LIBC_MALLOC             0
#endif
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    16384
// TCP_SND_QUEUELEN below comes out to ~65 with TCP_SND_BUF=16*MSS, so this
// must be >= that; sanity check in lwIP init.c errors out otherwise.
#define MEMP_NUM_TCP_SEG            96
#define MEMP_NUM_ARP_QUEUE          10
// rx pbuf pool. Must hold a full TLS record (~16KB + overhead).
#define PBUF_POOL_SIZE              32
#define PBUF_POOL_BUFSIZE           1700
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0     // raw sockets not needed for HTTPS
#define TCP_MSS                     1460
// TCP_WND must be larger than the largest TLS record (16KB + overhead) —
// with the default 4*MSS=5840 the receiver deadlocks: a 7KB TLS record
// can't fit in the window, server stops sending, mbedtls waits, no
// progress.
// Held at 12*MSS (~17.5KB) as a compromise: still above the 16KB TLS
// record bound (safe), but small enough that CloudFront's per-burst send
// can't grow as large as before. This reduces "send 22KB → wait → send
// 22KB" bursting toward smoother MSS-paced delivery. Going below 16KB
// requires negotiating TLS max_fragment_length first.
#define TCP_WND                     (12 * TCP_MSS)
#define TCP_SND_BUF                 (16 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETCONN                0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0     // HTTPS via IPv4 only; disable to save RAM
#define LWIP_IGMP                   0     // no multicast / mDNS
#define LWIP_TCP                    1
#define LWIP_UDP                    1     // required by DHCP + DNS
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// altcp / mbedTLS for HTTPS
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
// Force VERIFY_NONE (default is VERIFY_OPTIONAL which still runs cert checks
// and can stall when there is no CA / no system time).
//   0 == MBEDTLS_SSL_VERIFY_NONE
#define ALTCP_MBEDTLS_AUTHMODE      0

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif

#define ETHARP_DEBUG                LWIP_DBG_OFF
#define NETIF_DEBUG                 LWIP_DBG_OFF
#define PBUF_DEBUG                  LWIP_DBG_OFF
#define API_LIB_DEBUG               LWIP_DBG_OFF
#define API_MSG_DEBUG               LWIP_DBG_OFF
#define SOCKETS_DEBUG               LWIP_DBG_OFF
#define ICMP_DEBUG                  LWIP_DBG_OFF
#define INET_DEBUG                  LWIP_DBG_OFF
#define IP_DEBUG                    LWIP_DBG_OFF
#define IP_REASS_DEBUG              LWIP_DBG_OFF
#define RAW_DEBUG                   LWIP_DBG_OFF
#define MEM_DEBUG                   LWIP_DBG_OFF
#define MEMP_DEBUG                  LWIP_DBG_OFF
#define SYS_DEBUG                   LWIP_DBG_OFF
#define TCP_DEBUG                   LWIP_DBG_ON
#define TCP_INPUT_DEBUG             LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG            LWIP_DBG_OFF
#define TCP_RTO_DEBUG               LWIP_DBG_OFF
#define TCP_CWND_DEBUG              LWIP_DBG_OFF
#define TCP_WND_DEBUG               LWIP_DBG_OFF
#define TCP_FR_DEBUG                LWIP_DBG_OFF
#define TCP_QLEN_DEBUG              LWIP_DBG_OFF
#define TCP_RST_DEBUG               LWIP_DBG_OFF
#define UDP_DEBUG                   LWIP_DBG_OFF
#define TCPIP_DEBUG                 LWIP_DBG_ON
#define PPP_DEBUG                   LWIP_DBG_OFF
#define SLIP_DEBUG                  LWIP_DBG_OFF
#define DHCP_DEBUG                  LWIP_DBG_ON
#define ALTCP_MBEDTLS_DEBUG         LWIP_DBG_ON

#endif /* __LWIPOPTS_H__ */
