#pragma once

#include <array>

#include "main.h"
#include "platform/stm32h5/platform.h"

namespace board {


  using Platform = daveos::platform::stm32h5::Platform;
  inline constexpr const char* kName = "STM32H563";
  inline constexpr auto kDmaIrq = GPDMA1_Channel0_IRQn;
  extern std::array<std::uint8_t, 8192> tx_storage;
  // Start an asynchronous transfer from DMA-accessible, caller-owned storage.
  bool StartTransmit(const std::uint8_t* bytes, std::size_t size);
  std::uint32_t TimerClock();
  // LED indices are zero-based and must be less than three.
  void SetLed(std::size_t index, bool on);
  void ToggleLed(std::size_t index);
  // Raw button level: true means high, without polarity interpretation.
  bool ReadButton();


}  // namespace board
