#pragma once

#include <array>

#include "main.h"
#include "platform/stm32h7/platform.h"

namespace board {


using Platform = daveos::platform::stm32h7::Platform;
inline constexpr const char* kName = "STM32H755 M7";
inline constexpr auto kDmaIrq = DMA1_Stream0_IRQn;
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
