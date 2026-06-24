#pragma once

// ---------------------------------------------------------------------------
// lwIP options for Pico 2W foosball project
// ---------------------------------------------------------------------------

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000

#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     2
// Sized for: TCP + ARP + 2*DHCP + DNS baseline + MQTT keep-alive + spare.
// The auto-computed default of ~5 is not enough once MQTT is in the mix
// — the pool overflows and lwIP panics in sys_timeout.
#define MEMP_NUM_SYS_TIMEOUT        16
#define PBUF_POOL_SIZE              24

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_SNMP                   0
#define LWIP_IGMP                   0
#define LWIP_DNS                    1

#define LWIP_TCP                    1
#define TCP_TTL                     255
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)

#define LWIP_UDP                    1
#define LWIP_STATS                  0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

#define LWIP_SUPPORT_CUSTOM_PBUF    1
#define LWIP_TIMEVAL_PRIVATE        0

#define LWIP_CHKSUM_ALGORITHM       3

// ---------------------------------------------------------------------------
// MQTT client (publishes heartbeat/rfid/state/ball to the Pi broker)
// ---------------------------------------------------------------------------
#define LWIP_MQTT                   1
#define MQTT_OUTPUT_RINGBUF_SIZE    512
#define MQTT_VAR_HEADER_BUFFER_LEN  128
