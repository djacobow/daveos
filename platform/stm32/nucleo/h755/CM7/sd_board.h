#pragma once

#include "board_config.h"
#include "platform/stm32h7/spi_dma.h"

namespace board {


  using SdSpiBus = daveos::platform::stm32h7::SpiBus;
  using SdBusCritical = daveos::platform::stm32h7::BusCritical;

  // Dedicated DMA staging: written before use, so NOLOAD is intentional.
  alignas(32) inline daveos::platform::stm32h7::Spi1Dma::Buffers sd_dma_storage
      __attribute__((section(".dma_tx")));

  class SdDma {
   public:
    explicit SdDma(bool enabled) : engine_(sd_dma_storage), enabled_(enabled) {}

    auto operations() {
      return enabled_ ? engine_.operations()
                      : daveos::platform::stm32::detail::SpiDma{};
    }

   private:
    daveos::platform::stm32h7::Spi1Dma engine_;
    bool enabled_;
  };

  // SPI1 fixture: PA5 SCK, PB5 MOSI, PD14 GPIO CS; board-specific MISO.
  inline void PrepareSdMiso(GPIO_InitTypeDef gpio) {
    gpio.Pin = GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &gpio);
  }


}  // namespace board
