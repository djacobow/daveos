#pragma once

#include "net/service.h"

namespace daveos::net::stm32 {


// Single Ethernet MAC owned by the application. DMA buffers are placed in the
// board's .eth_dma section; the driver never calls the networking stack.
Driver ethernet_driver();
Config board_network_config();


}  // namespace daveos::net::stm32
