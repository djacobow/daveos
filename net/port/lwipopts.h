#pragma once

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_TCP 1
#define TCP_MSS 1460
#define TCP_WND 4096
#define TCP_SND_BUF 4096
#define TCP_SND_QUEUELEN 16
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_PCB_LISTEN 1
#define MEMP_NUM_TCP_SEG 24
#define TCP_LISTEN_BACKLOG 1
#define TCP_OVERSIZE 0
#define LWIP_IPV6 0
#define LWIP_DNS 0
#define LWIP_IPV4 1
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_DHCP 1
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_AUTOIP 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0
#define LWIP_NETIF_HOSTNAME 1
#define MEM_ALIGNMENT 8
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_USE_POOLS 1
#define MEMP_USE_CUSTOM_POOLS 1
#define MEM_USE_POOLS_TRY_BIGGER_POOL 1
#define MEMP_NUM_PBUF 16
#define MEMP_NUM_UDP_PCB 4
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1536
#define LWIP_STATS 1
#define MEM_STATS 1
#define MEMP_STATS 1
#define LWIP_RAND() daveos_net_random()
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t daveos_net_random(void);
#ifdef __cplusplus
}
#endif
