#pragma once

#include <algorithm>
#include <array>

#include "platform/stm32h7/bus.h"

namespace daveos::platform::stm32h7 {


  // Dedicated SPI1 DMA1 stream 1 RX / stream 2 TX, DMAMUX requests 37/38.
  // Caller reserves these streams and routes both IRQs to
  // Controller::interrupt. Buffers must be in DMA1-accessible SRAM, alive for
  // this object's lifetime. No DMA ever targets application spans (which may be
  // in inaccessible DTCM).
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
    static constexpr auto kRxFlags = DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 |
                                     DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 |
                                     DMA_LIFCR_CTCIF1;
    static constexpr auto kTxFlags = DMA_LIFCR_CFEIF2 | DMA_LIFCR_CDMEIF2 |
                                     DMA_LIFCR_CTEIF2 | DMA_LIFCR_CHTIF2 |
                                     DMA_LIFCR_CTCIF2;
    static constexpr auto kErrors = DMA_LISR_FEIF1 | DMA_LISR_DMEIF1 |
                                    DMA_LISR_TEIF1 | DMA_LISR_FEIF2 |
                                    DMA_LISR_DMEIF2 | DMA_LISR_TEIF2;

    hal::Status Init() {
      const auto address = reinterpret_cast<std::uintptr_t>(&buffers_);
      // This implementation deliberately supports the board's AXI SRAM only.
      if (address < 0x24000000 || address > 0x24080000 - sizeof(Buffers) ||
          address % 32) {
        return hal::Status::invalid_argument;
      }
      RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_DMA1EN;
      (void)RCC->AHB1ENR;
      __DSB();
      initialized_ = true;
      if (!Stop()) {
        return hal::Status::hardware_error;
      }
      for (auto irq : {DMA1_Stream1_IRQn, DMA1_Stream2_IRQn}) {
        NVIC_SetPriority(irq, (1u << __NVIC_PRIO_BITS) - 1);
        NVIC_ClearPendingIRQ(irq);
        NVIC_EnableIRQ(irq);
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
      if (SCB->CCR & SCB_CCR_DC_Msk) {
        SCB_CleanDCache_by_Addr(buffers_.tx.data(), buffers_.tx.size());
        SCB_CleanInvalidateDCache_by_Addr(buffers_.rx.data(),
                                          buffers_.rx.size());
      }
      DMAMUX1_Channel1->CCR = 37;
      DMAMUX1_Channel2->CCR = 38;
      DMA1_Stream1->PAR = reinterpret_cast<std::uintptr_t>(&SPI1->RXDR);
      DMA1_Stream1->M0AR = reinterpret_cast<std::uintptr_t>(buffers_.rx.data());
      DMA1_Stream1->NDTR = count;
      // Use FIFO mode for staged byte transfers.
      // Single memory/peripheral beats; full threshold also handles tails.
      DMA1_Stream1->FCR = DMA_SxFCR_DMDIS | DMA_SxFCR_FEIE | DMA_SxFCR_FTH;
      DMA1_Stream2->PAR = reinterpret_cast<std::uintptr_t>(&SPI1->TXDR);
      DMA1_Stream2->M0AR = reinterpret_cast<std::uintptr_t>(buffers_.tx.data());
      DMA1_Stream2->NDTR = count;
      DMA1_Stream2->FCR = DMA_SxFCR_DMDIS | DMA_SxFCR_FEIE | DMA_SxFCR_FTH;
      constexpr auto control =
          DMA_SxCR_MINC | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
      __DSB();
      NVIC_EnableIRQ(DMA1_Stream1_IRQn);
      NVIC_EnableIRQ(DMA1_Stream2_IRQn);
      DMA1_Stream1->CR = control | DMA_SxCR_EN;
      DMA1_Stream2->CR = control | DMA_SxCR_DIR_0 | DMA_SxCR_EN;
      return hal::Status::ok;
    }

    std::optional<hal::Status> Poll() {
      const auto flags = DMA1->LISR;
      if (flags & kErrors) {
        last_error_flags_ = flags;
        return hal::Status::hardware_error;
      }
      // A completed TX stream must not continuously re-pend while RX drains.
      if (flags & DMA_LISR_TCIF1) {
        NVIC_DisableIRQ(DMA1_Stream1_IRQn);
      }
      if (flags & DMA_LISR_TCIF2) {
        NVIC_DisableIRQ(DMA1_Stream2_IRQn);
      }
      if ((flags & (DMA_LISR_TCIF1 | DMA_LISR_TCIF2)) ==
          (DMA_LISR_TCIF1 | DMA_LISR_TCIF2)) {
        return hal::Status::ok;
      }
      return std::nullopt;
    }

    bool Stop() {
      if (!initialized_) {
        return true;
      }
      NVIC_DisableIRQ(DMA1_Stream1_IRQn);
      NVIC_DisableIRQ(DMA1_Stream2_IRQn);
      DMA1_Stream1->CR = DMA1_Stream1->CR & ~DMA_SxCR_EN;
      DMA1_Stream2->CR = DMA1_Stream2->CR & ~DMA_SxCR_EN;
      // Bounded quiescence check; staging isolates borrowed buffers even if a
      // wedged stream cannot stop. Never reset DMA1: UART owns stream 0.
      for (std::uint32_t i = 0; i < 1024; ++i) {
        if (!((DMA1_Stream1->CR | DMA1_Stream2->CR) & DMA_SxCR_EN)) {
          DMA1->LIFCR = kRxFlags | kTxFlags;
          NVIC_ClearPendingIRQ(DMA1_Stream1_IRQn);
          NVIC_ClearPendingIRQ(DMA1_Stream2_IRQn);
          return true;
        }
      }
      return false;
    }

    void Read(std::span<std::uint8_t> rx) {
      __DSB();
      if (SCB->CCR & SCB_CCR_DC_Msk) {
        SCB_InvalidateDCache_by_Addr(buffers_.rx.data(), buffers_.rx.size());
      }
      std::copy_n(buffers_.rx.begin(), rx.size(), rx.begin());
    }

    Buffers& buffers_;
    bool initialized_ = false;
    std::uint32_t last_error_flags_ = 0;
  };


}  // namespace daveos::platform::stm32h7
