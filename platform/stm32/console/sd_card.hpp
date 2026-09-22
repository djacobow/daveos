#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "board_config.h"
#include "hal/adapters/daveos.hpp"
#include "hal/controller.hpp"
#include "sd_board.h"
#include "storage/sd/session.h"

namespace daveos::platform::stm32 {


  // Nucleo SD fixture: SPI1 (H563 PA5/PG9/PB5, H755 PA5/PA6/PB5) with GPIO
  // chip select PD14 and optional DMA payloads, driving a storage::sd::Session.
  // Not a module: the owning module calls init() in stage1, tick() from a
  // task, and routes SPI1 (and DMA, if enabled) interrupts to interrupt().
  // Construction is passive. The session's pump yields to the scheduler, so
  // block reads/writes must come from a task call chain.
  template <typename Event>
  class SdCard {
    using Clock =
        hal::DaveOsClock<core::SchedulerInterface<Event>, board::Platform>;
    using Session = storage::sd::Session;

    // Keep a private config copy so startup can use <=400 kHz and the session
    // can switch rate between transactions, never while HAL owns the bus.
    class Bus : public board::SdSpiBus {
     public:
      explicit Bus(const detail::SpiDma& dma)
          : board::SdSpiBus(SPI1, SPI1_IRQn, HSI_VALUE,
                            {nullptr, Prepare, Reset, nullptr}, dma) {}

      hal::Status init(std::span<const Config> devices) {
        configs_[0] = devices[0];
        configs_[0].maximum_hz = Session::kStartupHz;
        return board::SdSpiBus::init(configs_);
      }

      void speed(std::uint32_t hz) { configs_[0].maximum_hz = hz; }

     private:
      std::array<Config, 1> configs_{};
    };

    using Controller = hal::Controller<Bus, Clock, board::SdBusCritical, 1>;

   public:
    struct Counters {
      std::uint32_t interrupts = 0, polls = 0, dma_chunks = 0, dma_bytes = 0;
    };

    SdCard(board::Platform& platform, bool dma,
           const Session::Observer& observer = {})
        : clock_(platform),
          dma_(dma),
          backend_(dma_.operations()),
          bus_(backend_, clock_, critical_,
               {{{{GPIOD, GPIO_PIN_14, false}, Session::kDataHz, 0, false}}}),
          session_(bus_.template device<0>(), SpeedHooks(), ResetHooks(),
                   PumpHook(), observer) {}

    SdCard(const SdCard&) = delete;
    SdCard& operator=(const SdCard&) = delete;

    // Stage1: bind the scheduler (timeouts and the pump) and set up SPI1.
    hal::Status init(core::SchedulerInterface<Event>& scheduler) {
      scheduler_ = &scheduler;
      clock_.bind(scheduler);
      return bus_.init();
    }

    void interrupt() {
      ++interrupts_;
      bus_.interrupt();
    }

    void tick() { session_.tick(); }

    Session& session() { return session_; }

    storage::BlockDevice block_device() { return session_.block_device(); }

    Counters counters() {
      critical_.enter();
      const auto spi = backend_.counters();
      const Counters result{interrupts_, spi.polls, spi.dma_chunks,
                            spi.dma_bytes};
      critical_.leave();
      return result;
    }

    bool faulted() const { return bus_.faulted(); }

    std::uint32_t rate() { return backend_.rate(0); }

    // For diagnostics that choose their own rate between transactions.
    void speed(std::uint32_t hz) { backend_.speed(hz); }

   private:
    Session::Speed SpeedHooks() {
      return {
          this,
          [](void* p, std::uint32_t hz) {
            static_cast<SdCard*>(p)->backend_.speed(hz);
          },
          [](void* p) { return static_cast<SdCard*>(p)->backend_.rate(0); }};
    }

    Session::Reset ResetHooks() {
      return {this, [](void* p, hal::Callback<hal::ResetResult> done) {
                return static_cast<SdCard*>(p)->bus_.reset(done);
              }};
    }

    storage::sd::Transport::Pump PumpHook() {
      return {this, [](void* context) {
                auto* scheduler = static_cast<SdCard*>(context)->scheduler_;
                if (!scheduler) {
                  return false;
                }
                const auto status = scheduler->yield();
                return status == core::Status::ok ||
                       status == core::Status::empty ||
                       status == core::Status::depth_limit;
              }};
    }

    static hal::Status Prepare(void*) {
      // CKPER uses HSI (64 MHz); /256 yields 250 kHz for SD startup.
      __HAL_RCC_GPIOA_CLK_ENABLE();
      __HAL_RCC_GPIOB_CLK_ENABLE();
      __HAL_RCC_GPIOG_CLK_ENABLE();
      __HAL_RCC_GPIOD_CLK_ENABLE();
      __HAL_RCC_CLKP_CONFIG(RCC_CLKPSOURCE_HSI);
      __HAL_RCC_SPI1_CONFIG(RCC_SPI1CLKSOURCE_CLKP);
      __HAL_RCC_SPI1_CLK_ENABLE();
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_14, GPIO_PIN_SET);
      GPIO_InitTypeDef gpio{};
      gpio.Pin = GPIO_PIN_14;
      gpio.Mode = GPIO_MODE_OUTPUT_PP;
      gpio.Pull = GPIO_NOPULL;
      gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
      HAL_GPIO_Init(GPIOD, &gpio);
      gpio.Mode = GPIO_MODE_AF_PP;
      gpio.Pull = GPIO_PULLUP;
      gpio.Alternate = GPIO_AF5_SPI1;
      gpio.Pin = GPIO_PIN_5;
      HAL_GPIO_Init(GPIOA, &gpio);
      HAL_GPIO_Init(GPIOB, &gpio);
      board::PrepareSdMiso(gpio);
      return hal::Status::ok;
    }

    static void Reset(void*) {
      __HAL_RCC_SPI1_FORCE_RESET();
      __DSB();
      __HAL_RCC_SPI1_RELEASE_RESET();
      __DSB();
    }

    Clock clock_;
    board::SdBusCritical critical_;
    std::uint32_t interrupts_ = 0;
    board::SdDma dma_;
    Bus backend_;
    Controller bus_;
    Session session_;
    core::SchedulerInterface<Event>* scheduler_ = nullptr;
  };


}  // namespace daveos::platform::stm32
