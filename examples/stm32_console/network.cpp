#include "network.h"

#include "platform/stm32/ethernet/driver.h"

namespace app {
  namespace net = daveos::net;

  Network::Network(Platform& platform)
      : service(net::stm32::ethernet_driver(),
                {&platform,
                 [](void* context) -> std::uint32_t {
                   return static_cast<Platform*>(context)->now() / 1000;
                 }}),
        // To use static IPv4, supply a factory setting dhcp=false and
        // addresses.
        module(service, net::stm32::board_network_config) {}

  void Network::stop() { service.stop(); }
}  // namespace app
