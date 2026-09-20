add_languages('c', native: false)
lwip_sources = files(
  'lwip/src/core/init.c', 'lwip/src/core/def.c', 'lwip/src/core/inet_chksum.c',
  'lwip/src/core/ip.c', 'lwip/src/core/mem.c', 'lwip/src/core/memp.c',
  'lwip/src/core/netif.c', 'lwip/src/core/pbuf.c', 'lwip/src/core/stats.c',
  'lwip/src/core/tcp.c', 'lwip/src/core/tcp_in.c', 'lwip/src/core/tcp_out.c',
  'lwip/src/core/sys.c', 'lwip/src/core/timeouts.c', 'lwip/src/core/udp.c',
  'lwip/src/core/ipv4/dhcp.c', 'lwip/src/core/ipv4/etharp.c',
  'lwip/src/core/ipv4/icmp.c', 'lwip/src/core/ipv4/ip4.c',
  'lwip/src/core/ipv4/ip4_addr.c', 'lwip/src/netif/ethernet.c')
network_includes = include_directories('..', 'port', 'lwip/src/include')
network_lib = static_library('daveos-net', lwip_sources, 'service.cpp', 'tcp_server.cpp',
  include_directories: network_includes)
network_dep = declare_dependency(link_with: network_lib,
  include_directories: include_directories('..'))
# OTA is an optional adapter; the base network library remains independent.
network_update_lib = static_library('daveos-net-update', 'update.cpp',
  dependencies: [network_dep, update_dep])
network_update_dep = declare_dependency(link_with: network_update_lib,
  dependencies: [network_dep, update_dep])
