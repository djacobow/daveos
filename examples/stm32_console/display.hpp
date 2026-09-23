#pragma once

#include "drivers/adapters/ssd1306.hpp"
#include "i2c_probe.hpp"

namespace app {


  using Display = daveos::drivers::Ssd1306Module<I2cBus, Event>;


}  // namespace app
