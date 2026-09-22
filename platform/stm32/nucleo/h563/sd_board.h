#pragma once

#include "board_config.h"
#include "platform/stm32h5/spi_dma.h"

namespace board {


  using SdSpiBus = daveos::platform::stm32h5::SpiBus;
  using SdBusCritical = daveos::platform::stm32h5::BusCritical;

  class SdDma {
   public:
    explicit SdDma(bool enabled) : engine_(buffers_), enabled_(enabled) {}

    auto operations() {
      return enabled_ ? engine_.operations()
                      : daveos::platform::stm32::detail::SpiDma{};
    }

   private:
    daveos::platform::stm32h5::Spi1Dma::Buffers buffers_;
    daveos::platform::stm32h5::Spi1Dma engine_;
    bool enabled_;
  };

  // SPI1 fixture: PA5 SCK, PB5 MOSI, PD14 GPIO CS; board-specific MISO.
  inline void PrepareSdMiso(GPIO_InitTypeDef gpio) {
    gpio.Pin = GPIO_PIN_9;
    HAL_GPIO_Init(GPIOG, &gpio);
  }


}  // namespace board
