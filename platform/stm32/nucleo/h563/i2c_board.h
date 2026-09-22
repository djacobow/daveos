#pragma once

#include "board_config.h"
#include "platform/stm32h5/bus.h"

namespace board {
  using I2cBackend = daveos::platform::stm32h5::I2cBus;
  using I2cCritical = daveos::platform::stm32h5::BusCritical;
  using I2cRecoveryPins = daveos::platform::stm32h5::I2cRecoveryPins;
}  // namespace board
