#pragma once

// The shared implementation requires the selected device's register types.
// clang-format off
#include "stm32h7xx.h"
#include "platform/stm32/bus/driver.h"

// clang-format on

namespace daveos::platform::stm32h7 {


  using BusCritical = stm32::detail::BusCritical;
  using BusHardware = stm32::detail::BusHardware;
  using SpiConfig = stm32::detail::SpiConfig;
  using SpiBus = stm32::detail::SpiBus;
  using I2cConfig = stm32::detail::I2cConfig;
  using I2cBus = stm32::detail::I2cBus;


}  // namespace daveos::platform::stm32h7
