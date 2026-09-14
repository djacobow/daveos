#include "ethernet_board.h"

extern "C" void HAL_ETH_MspInit(ETH_HandleTypeDef*) {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_ETH_CLK_ENABLE();
  __HAL_RCC_ETHTX_CLK_ENABLE();
  __HAL_RCC_ETHRX_CLK_ENABLE();
  __HAL_RCC_ETH_CLK_SLEEP_ENABLE();
  __HAL_RCC_ETHTX_CLK_SLEEP_ENABLE();
  __HAL_RCC_ETHRX_CLK_SLEEP_ENABLE();
  GPIO_InitTypeDef pins{};
  pins.Mode = GPIO_MODE_AF_PP;
  pins.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  pins.Alternate = GPIO_AF11_ETH;
  pins.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
  HAL_GPIO_Init(GPIOA, &pins);
  pins.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
  HAL_GPIO_Init(GPIOC, &pins);
  pins.Pin = GPIO_PIN_11 | GPIO_PIN_13;
  HAL_GPIO_Init(GPIOG, &pins);
  pins.Pin = GPIO_PIN_15;
  HAL_GPIO_Init(GPIOB, &pins);
  HAL_NVIC_SetPriority(ETH_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(ETH_IRQn);
}
extern "C" void HAL_ETH_MspDeInit(ETH_HandleTypeDef*) {
  HAL_NVIC_DisableIRQ(ETH_IRQn);
  // Leave clocks/pins configured so MDIO link polling works with MAC stopped.
}
