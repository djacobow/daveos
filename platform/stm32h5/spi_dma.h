#pragma once

#include <algorithm>
#include <array>

#include "platform/stm32h5/bus.h"

namespace daveos::platform::stm32h5 {


  // SPI1 GPDMA1 channel 1 RX / channel 2 TX, requests 6/7. Reserve both
  // channels and route their IRQs to Controller::interrupt. Channel 0 (UART)
  // is untouched. Staging must reside in nonsecure SRAM and outlive this
  // adapter; no DMA accesses borrowed application spans. H563 has no D-cache.
  class Spi1Dma {
   public:
    struct alignas(32) Buffers {
      std::array<std::uint8_t, 512> tx, rx;
    };

    explicit Spi1Dma(Buffers& buffers) : buffers_(buffers) {}

    stm32::detail::SpiDma operations() {
      return {this,
              [](void* p) { return static_cast<Spi1Dma*>(p)->Init(); },
              [](void* p, std::span<const std::uint8_t> tx, std::size_t size,
                 std::uint8_t fill) {
                return static_cast<Spi1Dma*>(p)->Start(tx, size, fill);
              },
              [](void* p) { return static_cast<Spi1Dma*>(p)->Poll(); },
              [](void* p) { return static_cast<Spi1Dma*>(p)->Stop(); },
              [](void* p, std::span<std::uint8_t> rx) {
                static_cast<Spi1Dma*>(p)->Read(rx);
              },
              buffers_.tx.size()};
    }

   private:
    static constexpr auto kErrors =
        DMA_CSR_DTEF | DMA_CSR_ULEF | DMA_CSR_USEF | DMA_CSR_TOF;
    static constexpr auto kFlags =
        kErrors | DMA_CSR_TCF | DMA_CSR_HTF | DMA_CSR_SUSPF;
    static constexpr auto kControl = DMA_CCR_TCIE | DMA_CCR_DTEIE |
                                     DMA_CCR_ULEIE | DMA_CCR_USEIE |
                                     DMA_CCR_TOIE;

    hal::Status Init() {
      const auto address = reinterpret_cast<std::uintptr_t>(&buffers_);
      if (address < 0x20000000 || address > 0x200a0000 - sizeof(Buffers) ||
          address % 32) {
        return hal::Status::invalid_argument;
      }
      RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_GPDMA1EN;
      (void)RCC->AHB1ENR;
      __DSB();
      initialized_ = true;
      if (!Stop()) {
        return hal::Status::hardware_error;
      }
      for (auto irq : {GPDMA1_Channel1_IRQn, GPDMA1_Channel2_IRQn}) {
        NVIC_SetPriority(irq, (1u << __NVIC_PRIO_BITS) - 1);
        NVIC_ClearPendingIRQ(irq);
      }
      return hal::Status::ok;
    }

    hal::Status Start(std::span<const std::uint8_t> tx, std::size_t count,
                      std::uint8_t fill) {
      if (!initialized_ || count == 0 || count > buffers_.tx.size() ||
          (!tx.empty() && tx.size() != count)) {
        return hal::Status::invalid_argument;
      }
      if (!Stop()) {
        return hal::Status::hardware_error;
      }
      if (tx.empty()) {
        std::fill_n(buffers_.tx.begin(), count, fill);
      } else {
        std::copy(tx.begin(), tx.end(), buffers_.tx.begin());
      }
      // Single-byte beats/bursts. Port 0 for APB SPI, port 1 for SRAM.
      GPDMA1_Channel1->CTR1 = DMA_CTR1_DINC | DMA_CTR1_DAP;
      GPDMA1_Channel1->CTR2 = 6;
      GPDMA1_Channel1->CBR1 = count;
      GPDMA1_Channel1->CSAR = reinterpret_cast<std::uintptr_t>(&SPI1->RXDR);
      GPDMA1_Channel1->CDAR =
          reinterpret_cast<std::uintptr_t>(buffers_.rx.data());
      GPDMA1_Channel1->CLLR = 0;
      GPDMA1_Channel2->CTR1 = DMA_CTR1_SINC | DMA_CTR1_SAP;
      GPDMA1_Channel2->CTR2 = 7 | DMA_CTR2_DREQ;
      GPDMA1_Channel2->CBR1 = count;
      GPDMA1_Channel2->CSAR =
          reinterpret_cast<std::uintptr_t>(buffers_.tx.data());
      GPDMA1_Channel2->CDAR = reinterpret_cast<std::uintptr_t>(&SPI1->TXDR);
      GPDMA1_Channel2->CLLR = 0;
      __DSB();
      NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
      NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
      GPDMA1_Channel1->CCR = kControl | DMA_CCR_EN;
      GPDMA1_Channel2->CCR = kControl | DMA_CCR_EN;
      return hal::Status::ok;
    }

    std::optional<hal::Status> Poll() {
      const auto rx = GPDMA1_Channel1->CSR;
      const auto tx = GPDMA1_Channel2->CSR;
      if ((rx | tx) & kErrors) {
        last_error_flags_ = rx | tx;
        return hal::Status::hardware_error;
      }
      // Preserve completion flags until both channels and SPI EOT finish,
      // without repeated IRQs from the channel that completed first.
      if (rx & DMA_CSR_TCF) {
        NVIC_DisableIRQ(GPDMA1_Channel1_IRQn);
      }
      if (tx & DMA_CSR_TCF) {
        NVIC_DisableIRQ(GPDMA1_Channel2_IRQn);
      }
      if ((rx & tx) & DMA_CSR_TCF) {
        return hal::Status::ok;
      }
      return std::nullopt;
    }

    static bool StopChannel(DMA_Channel_TypeDef* channel) {
      // RM0481: suspend an enabled channel, wait for quiescence, then reset.
      // Do not set SUSP on a disabled channel or carry it into the next start.
      if (channel->CCR & DMA_CCR_EN) {
        // EN ignores a written zero. Never copy its sampled value back:
        // hardware may have cleared it on completion since the read.
        channel->CCR = DMA_CCR_SUSP;
        std::uint32_t remaining = 1024;
        while (!(channel->CSR & DMA_CSR_SUSPF) && (channel->CCR & DMA_CCR_EN) &&
               remaining) {
          --remaining;
        }
        if (!(channel->CSR & DMA_CSR_SUSPF) && (channel->CCR & DMA_CCR_EN)) {
          return false;
        }
      }
      channel->CCR = DMA_CCR_RESET;
      __DSB();
      if (channel->CCR & (DMA_CCR_EN | DMA_CCR_SUSP | DMA_CCR_RESET)) {
        return false;
      }
      channel->CFCR = kFlags;
      return true;
    }

    bool Stop() {
      if (!initialized_) {
        return true;
      }
      NVIC_DisableIRQ(GPDMA1_Channel1_IRQn);
      NVIC_DisableIRQ(GPDMA1_Channel2_IRQn);
      // Attempt both even if one fails. Never reset all of GPDMA1 (UART).
      const bool rx = StopChannel(GPDMA1_Channel1);
      const bool tx = StopChannel(GPDMA1_Channel2);
      NVIC_ClearPendingIRQ(GPDMA1_Channel1_IRQn);
      NVIC_ClearPendingIRQ(GPDMA1_Channel2_IRQn);
      return rx && tx;
    }

    void Read(std::span<std::uint8_t> rx) {
      __DSB();
      std::copy_n(buffers_.rx.begin(), rx.size(), rx.begin());
    }

    Buffers& buffers_;
    bool initialized_ = false;
    std::uint32_t last_error_flags_ = 0;
  };


}  // namespace daveos::platform::stm32h5
