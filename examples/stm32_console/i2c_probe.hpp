#pragma once

#include <cinttypes>

#include "application.h"
#include "hal/adapters/daveos.hpp"
#include "hal/adapters/i2c_module.hpp"
#include "hal/controller.hpp"
#include "platform/stm32h5/bus.h"

namespace app {


  // H563 I2C1 owner. A task-time lease excludes scans
  // across a client sequence of transactions, including conversion waits.
  class I2cBus {
    using Status = daveos::hal::Status;
    using Clock =
        daveos::hal::DaveOsClock<core::SchedulerInterface<Event>, Platform>;

    using Backend = daveos::platform::stm32h5::I2cBus;
    using Critical = daveos::platform::stm32h5::BusCritical;
    using Bus = daveos::hal::Controller<Backend, Clock, Critical, 1>;
    static constexpr std::uint32_t kPins = GPIO_PIN_8 | GPIO_PIN_9;
    // HSI 64 MHz, PRESC=15: 250 ns timing ticks. SCL low=6 us,
    // high=5 us, setup=1.25 us, hold=0.5 us; analog filter enabled.
    // Synchronization/filter/rise time further reduce SCL below 91 kHz.
    static constexpr std::uint32_t kTiming = 0xf0421317;

   public:
    explicit I2cBus(Platform& platform)
        : clock_(platform),
          backend_(I2C1, I2C1_EV_IRQn, I2C1_ER_IRQn, kTiming, 80000,
                   {nullptr, Prepare, Reset, Idle},
                   recovery_pins_.operations()),
          bus_(backend_, clock_, critical_, {{{{0x68}}}}) {}

    static constexpr const char* name() { return "I2C1"; }

    // Wiring takes addresses only; no construction-order dependency.
    static daveos::hal::i2c::Device device(I2cBus& owner) {
      return Bus::bind<0>(owner.bus_);
    }

    // Task context only. The caller retains its lease until all transfers
    // complete; scans and resets cannot interrupt the client sequence.
    bool acquire() {
      if (leased_) {
        return false;
      }
      leased_ = true;
      return true;
    }

    void release() { leased_ = false; }

    Status init(core::SchedulerInterface<Event>& scheduler) {
      clock_.bind(scheduler);
      return bus_.init();
    }

    void deinit() { bus_.deinit(); }

    void interrupt() { bus_.interrupt(); }

    auto statistics() { return bus_.statistics(); }

    Status probe(daveos::hal::i2c::Address address,
                 daveos::hal::i2c::Callback callback,
                 std::optional<daveos::hal::Duration> timeout) {
      return bus_.probe(address, callback, timeout);
    }

    Status reset(daveos::hal::Callback<daveos::hal::ResetResult> callback,
                 std::optional<daveos::hal::Duration> timeout) {
      return bus_.reset(callback, timeout);
    }

    bool needs_reset() const { return bus_.faulted(); }

   private:
    static Status Prepare(void*) {
      __HAL_RCC_GPIOB_CLK_ENABLE();
      __HAL_RCC_I2C1_CONFIG(RCC_I2C1CLKSOURCE_HSI);
      __HAL_RCC_I2C1_CLK_ENABLE();
      GPIO_InitTypeDef gpio{};
      gpio.Pin = kPins;
      gpio.Mode = GPIO_MODE_AF_OD;
      gpio.Pull = GPIO_NOPULL;
      gpio.Speed = GPIO_SPEED_FREQ_LOW;
      gpio.Alternate = GPIO_AF4_I2C1;
      HAL_GPIO_Init(GPIOB, &gpio);
      return Status::ok;
    }

    static void Reset(void*) {
      __HAL_RCC_I2C1_FORCE_RESET();
      __DSB();
      __HAL_RCC_I2C1_RELEASE_RESET();
      __DSB();
    }

    static bool Idle(void*) { return (GPIOB->IDR & kPins) == kPins; }

    Clock clock_;
    daveos::platform::stm32h5::I2cRecoveryPins recovery_pins_{
        {GPIOB, GPIO_PIN_8}, {GPIOB, GPIO_PIN_9}};
    Backend backend_;
    Critical critical_;
    Bus bus_;
    bool leased_ = false;
  };

  using I2cProbe = daveos::hal::I2cModule<Event, I2cBus>;


}  // namespace app
