add_languages('c', native: false)
lwip_sources = files(
  '../../third_party/lwip/src/core/init.c', '../../third_party/lwip/src/core/def.c', '../../third_party/lwip/src/core/inet_chksum.c',
  '../../third_party/lwip/src/core/ip.c', '../../third_party/lwip/src/core/mem.c', '../../third_party/lwip/src/core/memp.c',
  '../../third_party/lwip/src/core/netif.c', '../../third_party/lwip/src/core/pbuf.c', '../../third_party/lwip/src/core/stats.c',
  '../../third_party/lwip/src/core/tcp.c', '../../third_party/lwip/src/core/tcp_in.c', '../../third_party/lwip/src/core/tcp_out.c',
  '../../third_party/lwip/src/core/sys.c', '../../third_party/lwip/src/core/timeouts.c', '../../third_party/lwip/src/core/udp.c',
  '../../third_party/lwip/src/core/ipv4/dhcp.c', '../../third_party/lwip/src/core/ipv4/etharp.c',
  '../../third_party/lwip/src/core/ipv4/icmp.c', '../../third_party/lwip/src/core/ipv4/ip4.c',
  '../../third_party/lwip/src/core/ipv4/ip4_addr.c', '../../third_party/lwip/src/netif/ethernet.c')
network_includes = include_directories('..', 'port', '../../third_party/lwip/src/include')
network_lib = static_library('daveos-net', lwip_sources, 'service.cpp', 'tcp_server.cpp',
  include_directories: network_includes)
network_dep = declare_dependency(link_with: network_lib,
  include_directories: include_directories('..'))
# OTA is an optional adapter; the base network library remains independent.
network_update_lib = static_library('daveos-net-update', 'update.cpp',
  dependencies: [network_dep, update_dep])
network_update_dep = declare_dependency(link_with: network_update_lib,
  dependencies: [network_dep, update_dep])
