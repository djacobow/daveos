#include "ethernet_board.h"

extern "C" void HAL_ETH_MspInit(ETH_HandleTypeDef*) {
  // Reserve MPU region 7 for the linker-reserved noncacheable DMA arena.
  HAL_MPU_Disable();
  MPU_Region_InitTypeDef region{};
  region.Enable = MPU_REGION_ENABLE;
  region.Number = MPU_REGION_NUMBER7;
  region.BaseAddress = 0x24070000;
  region.Size = MPU_REGION_SIZE_64KB;
  region.TypeExtField = MPU_TEX_LEVEL1;
  region.AccessPermission = MPU_REGION_FULL_ACCESS;
  region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  region.IsShareable = MPU_ACCESS_SHAREABLE;
  HAL_MPU_ConfigRegion(&region);
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_ETH1MAC_CLK_ENABLE();
  __HAL_RCC_ETH1TX_CLK_ENABLE();
  __HAL_RCC_ETH1RX_CLK_ENABLE();
  __HAL_RCC_ETH1MAC_CLK_SLEEP_ENABLE();
  __HAL_RCC_ETH1TX_CLK_SLEEP_ENABLE();
  __HAL_RCC_ETH1RX_CLK_SLEEP_ENABLE();
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
  pins.Pin = GPIO_PIN_13;
  HAL_GPIO_Init(GPIOB, &pins);
  HAL_NVIC_SetPriority(ETH_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(ETH_IRQn);
}

extern "C" void HAL_ETH_MspDeInit(ETH_HandleTypeDef*) {
  HAL_NVIC_DisableIRQ(ETH_IRQn);
  // Leave clocks/pins configured so MDIO link polling works with MAC stopped.
}
