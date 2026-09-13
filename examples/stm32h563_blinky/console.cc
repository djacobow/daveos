#include "board_config.h"
extern "C" UART_HandleTypeDef huart3;
namespace board {
// H7 requires AXI SRAM; H5's normal SRAM is GPDMA-accessible (no D-cache).
alignas(32) std::array<std::uint8_t, 8192> tx_storage{};
bool StartTransmit(const std::uint8_t* bytes, std::size_t size) {
  __DSB();
  if (HAL_UART_Transmit_DMA(&huart3, bytes, static_cast<std::uint16_t>(size)) !=
      HAL_OK)
    return false;
  __HAL_DMA_DISABLE_IT(huart3.hdmatx, DMA_IT_HT);
  return true;
}
std::uint32_t TimerClock() {
  __HAL_RCC_TIMCLKPRESCALER(RCC_TIMPRES_DEACTIVATED);
  RCC_ClkInitTypeDef clocks{};
  std::uint32_t latency;
  HAL_RCC_GetClockConfig(&clocks, &latency);
  auto hz = HAL_RCC_GetPCLK1Freq();
  if (clocks.APB1CLKDivider != RCC_HCLK_DIV1) hz *= 2;
  return hz;
}
}  // namespace board
