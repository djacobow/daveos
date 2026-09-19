#pragma once

#include "board_config.h"
#include "net/module.hpp"
#include "platform/stm32/ethernet/driver.h"

namespace daveos::platform::stm32 {


  // Nucleo Ethernet wiring. The service itself remains independent of DaveOS;
  // the module supplies polling. Stop consoles before stopping this service.
  template <typename Event>
  struct Network {
    explicit Network(board::Platform& platform)
        : service(net::stm32::ethernet_driver(),
                  {&platform,
                   [](void* context) -> std::uint32_t {
                     return static_cast<board::Platform*>(context)->now() /
                            1000;
                   }}),
          module(service, net::stm32::board_network_config) {}

    void stop() { service.stop(); }

    net::Service service;
    net::Module<Event> module;
  };


}  // namespace daveos::platform::stm32
