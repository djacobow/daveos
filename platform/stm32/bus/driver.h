#pragma once

// Include the selected device CMSIS header before this internal header.
#include <algorithm>
#include <array>
#include <bit>
#include <optional>

#include "hal/detail/actions.hpp"
#include "hal/i2c/bus_clear.h"

namespace daveos::platform::stm32::detail {


  // Per-controller critical domain. Interrupt masking nests and never waits;
  // bus IRQs must use the same preemption priority as the injected timer IRQ.
  class BusCritical {
   public:
    bool try_enter() {
      enter();
      return true;
    }

    void enter() {
      const auto mask = __get_PRIMASK();
      __disable_irq();
      if (depth_++ == 0) {
        previous_ = mask;
      }
    }

    void leave() {
      if (--depth_ == 0) {
        __set_PRIMASK(previous_);
      }
    }

   private:
    std::uint32_t depth_ = 0, previous_ = 0;
  };

  struct Pin {
    GPIO_TypeDef* port = nullptr;
    std::uint16_t mask = 0;
    bool active_high = false;

    bool valid() const {
      const bool known = port == GPIOA || port == GPIOB || port == GPIOC ||
                         port == GPIOD || port == GPIOE || port == GPIOF ||
                         port == GPIOG || port == GPIOH || port == GPIOI
#if defined(GPIOJ)
                         || port == GPIOJ
#endif
#if defined(GPIOK)
                         || port == GPIOK
#endif
          ;
      return known && mask && (mask & (mask - 1)) == 0;
    }

    void select(bool active) const {
      port->BSRR = active == active_high ? mask : std::uint32_t{mask} << 16;
    }

    bool same_pin(const Pin& other) const {
      return port == other.port && mask == other.mask;
    }
  };

  // Board code owns clocks, alternate-function assignments and RCC reset.
  // prepare may perform task-time setup. reset must be a bounded RCC reset
  // pulse, with no waits or callbacks. idle checks external I2C line levels.
  struct BusHardware {
    void* context = nullptr;
    hal::Status (*prepare)(void*) = nullptr;
    void (*reset)(void*) = nullptr;
    bool (*idle)(void*) = nullptr;
  };

  struct SpiConfig {
    Pin cs;
    std::uint32_t maximum_hz = 100000;
    std::uint8_t mode = 0;
    bool lsb_first = false;
  };

  // IRQ-driven, 8-bit, full-duplex SPI. Reads synthesize fill bytes and writes
  // discard RX in the IRQ; no bounce buffer or vendor polling routine.
  class SpiBus {
   public:
    using Action = hal::spi::Action;
    using Config = SpiConfig;

    constexpr SpiBus(SPI_TypeDef* registers, IRQn_Type irq,
                     std::uint32_t kernel_hz, const BusHardware& hardware)
        : registers_(registers),
          irq_(irq),
          kernel_hz_(kernel_hz),
          hardware_(hardware) {}

    const void* identity() const { return registers_; }

    static hal::PinIdentity chip_select(const Config& config) {
      return {reinterpret_cast<std::uintptr_t>(config.cs.port), config.cs.mask};
    }

    hal::Status validate(std::span<const Config> devices) const {
      invalid_device_.reset();
      const bool instance = (registers_ == SPI1 && irq_ == SPI1_IRQn) ||
                            (registers_ == SPI2 && irq_ == SPI2_IRQn) ||
                            (registers_ == SPI3 && irq_ == SPI3_IRQn) ||
                            (registers_ == SPI4 && irq_ == SPI4_IRQn) ||
                            (registers_ == SPI5 && irq_ == SPI5_IRQn) ||
                            (registers_ == SPI6 && irq_ == SPI6_IRQn);
      if (!instance || !kernel_hz_ || !hardware_.prepare || !hardware_.reset ||
          devices.empty()) {
        return hal::Status::invalid_argument;
      }
      for (std::size_t i = 0; i < devices.size(); ++i) {
        invalid_device_ = i;
        const auto& d = devices[i];
        if (!d.cs.valid() || d.mode > 3 || !d.maximum_hz ||
            (std::uint64_t{kernel_hz_} + 255) / 256 > d.maximum_hz) {
          return hal::Status::invalid_argument;
        }
        for (std::size_t j = 0; j < i; ++j) {
          if (d.cs.same_pin(devices[j].cs)) {
            return hal::Status::invalid_argument;
          }
        }
      }
      invalid_device_.reset();
      return hal::Status::ok;
    }

    std::optional<std::size_t> invalid_device() const {
      return invalid_device_;
    }

    hal::Status init(std::span<const Config> devices) {
      devices_ = devices;
      prepared_ = true;
      const auto status = hardware_.prepare(hardware_.context);
      if (status != hal::Status::ok) {
        return status;
      }
      hardware_.reset(hardware_.context);
      for (const auto& d : devices_) {
        d.cs.select(false);
      }
      NVIC_SetPriority(irq_, (1u << __NVIC_PRIO_BITS) - 1);
      NVIC_ClearPendingIRQ(irq_);
      NVIC_EnableIRQ(irq_);
      return hal::Status::ok;
    }

    void disable() {
      NVIC_DisableIRQ(irq_);
      if (prepared_) {
        (void)finish();
      }
    }

    std::uint32_t rate(std::size_t device) const {
      return kernel_hz_ / (2u << Prescaler(devices_[device].maximum_hz));
    }

    hal::Status validate_action(const Action&) const { return hal::Status::ok; }

    hal::Status begin(std::size_t device, std::span<const Action> actions) {
      device_ = device;
      Configure();
      selected_ = actions.front().operation != hal::spi::Operation::idle_clocks;
      devices_[device].cs.select(selected_);
      return hal::Status::ok;
    }

    hal::Status start(const Action& action, bool, bool) {
      action_ = action;
      total_ = action.operation == hal::spi::Operation::idle_clocks
                   ? action.amount / 8
                   : std::max(action.tx.size(), action.rx.size());
      sent_ = received_ = 0;
      active_ = true;
      Chunk();
      return hal::Status::ok;
    }

    std::optional<hal::Status> poll() {
      if (!active_) {
        return std::nullopt;
      }
      constexpr auto errors =
          SPI_SR_OVR | SPI_SR_UDR | SPI_SR_MODF | SPI_SR_TIFRE;
      // Bounded FIFO work. Hardware reasserts the IRQ if bytes remain ready.
      for (std::size_t i = 0; i < 32; ++i) {
        const auto flags = registers_->SR;
        if (flags & errors) {
          active_ = false;
          return hal::Status::hardware_error;
        }
        bool progress = false;
        if ((flags & SPI_SR_RXP) && received_ < chunk_end_) {
          const auto byte =
              *reinterpret_cast<volatile std::uint8_t*>(&registers_->RXDR);
          if (!action_.rx.empty()) {
            action_.rx[received_] = byte;
          }
          ++received_;
          progress = true;
        }
        if ((flags & SPI_SR_TXP) && sent_ < chunk_end_) {
          const auto byte =
              action_.tx.empty() ? action_.fill : action_.tx[sent_];
          *reinterpret_cast<volatile std::uint8_t*>(&registers_->TXDR) = byte;
          ++sent_;
          progress = true;
        }
        if (!progress) {
          break;
        }
      }
      if (sent_ == chunk_end_) {
        registers_->IER = registers_->IER & ~SPI_IER_TXPIE;
      }
      if ((registers_->SR & SPI_SR_EOT) && received_ == chunk_end_) {
        registers_->IER = 0;
        registers_->CR1 = SPI_CR1_SSI;
        registers_->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
        if (received_ < total_) {
          Chunk();
          return std::nullopt;
        }
        active_ = false;
        return hal::Status::ok;
      }
      return std::nullopt;
    }

    // RCC reset terminates even a wedged peripheral without waiting for SUSP.
    // IRQ-only backend: no independent DMA bus master retains caller buffers.
    hal::Status finish() {
      registers_->IER = 0;
      active_ = false;
      action_ = {};
      // Reset can release alternate-function pin control. Make CS inactive
      // first so any resulting SCK edge cannot be consumed by the device.
      for (const auto& d : devices_) {
        d.cs.select(false);
      }
      __DSB();
      hardware_.reset(hardware_.context);
      selected_ = false;
      NVIC_ClearPendingIRQ(irq_);
      return hal::Status::ok;
    }

    hal::Status reset() { return finish(); }

    void pend() { NVIC_SetPendingIRQ(irq_); }

   private:
    std::uint32_t Prescaler(std::uint32_t maximum) const {
      std::uint32_t shift = 0;
      while (shift < 7 &&
             (std::uint64_t{kernel_hz_} + (2u << shift) - 1) / (2u << shift) >
                 maximum) {
        ++shift;
      }
      return shift;
    }

    void Configure() {
      const auto& d = devices_[device_];
      registers_->CR1 = SPI_CR1_SSI;
      registers_->CFG1 = 7u | (Prescaler(d.maximum_hz) << SPI_CFG1_MBR_Pos);
      registers_->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR |
                         (d.mode & 1 ? SPI_CFG2_CPHA : 0) |
                         (d.mode & 2 ? SPI_CFG2_CPOL : 0) |
                         (d.lsb_first ? SPI_CFG2_LSBFRST : 0);
    }

    void Chunk() {
      chunk_end_ =
          received_ + std::min<std::uint64_t>(total_ - received_, 65535);
      registers_->CR2 = static_cast<std::uint32_t>(chunk_end_ - received_);
      registers_->IFCR = 0xffffffffu;
      registers_->IER = SPI_IER_RXPIE | SPI_IER_TXPIE | SPI_IER_EOTIE |
                        SPI_IER_OVRIE | SPI_IER_UDRIE | SPI_IER_MODFIE |
                        SPI_IER_TIFREIE;
      registers_->CR1 = SPI_CR1_SSI | SPI_CR1_SPE;
      registers_->CR1 = SPI_CR1_SSI | SPI_CR1_SPE | SPI_CR1_CSTART;
    }

    SPI_TypeDef* registers_;
    IRQn_Type irq_;
    std::uint32_t kernel_hz_;
    BusHardware hardware_;
    mutable std::optional<std::size_t> invalid_device_;
    std::span<const Config> devices_;
    Action action_{};
    std::uint64_t total_ = 0, chunk_end_ = 0, sent_ = 0, received_ = 0;
    std::size_t device_ = 0;
    bool prepared_ = false, selected_ = false, active_ = false;
  };

  // GPIO recovery adapter; configure pin clocks/pulls in BusHardware::prepare.
  // Both pins must belong exclusively to this single-controller I2C bus.
  class I2cRecoveryPins {
   public:
    constexpr I2cRecoveryPins(Pin scl, Pin sda) : scl_(scl), sda_(sda) {}

    hal::i2c::BusClearPins operations() {
      if (!scl_.valid() || !sda_.valid() || scl_.same_pin(sda_)) {
        return {};
      }
      return {this,
              [](void* p) { static_cast<I2cRecoveryPins*>(p)->Enter(); },
              [](void* p) { static_cast<I2cRecoveryPins*>(p)->Leave(); },
              [](void* p, bool release) {
                Drive(static_cast<I2cRecoveryPins*>(p)->scl_, release);
              },
              [](void* p, bool release) {
                Drive(static_cast<I2cRecoveryPins*>(p)->sda_, release);
              },
              [](void* p) {
                const auto& pin = static_cast<I2cRecoveryPins*>(p)->scl_;
                return (pin.port->IDR & pin.mask) != 0;
              },
              [](void* p) {
                const auto& pin = static_cast<I2cRecoveryPins*>(p)->sda_;
                return (pin.port->IDR & pin.mask) != 0;
              }};
    }

   private:
    static std::uint32_t Shift(const Pin& pin) {
      return 2 * std::countr_zero(static_cast<std::uint32_t>(pin.mask));
    }

    static void Drive(const Pin& pin, bool release) {
      pin.port->BSRR = release ? pin.mask : std::uint32_t{pin.mask} << 16;
    }

    static void Mode(const Pin& pin, std::uint32_t mode) {
      const auto shift = Shift(pin);
      pin.port->MODER = (pin.port->MODER & ~(3u << shift)) | (mode << shift);
    }

    void Enter() {
      // Preload released levels and open-drain before changing pin mode.
      Drive(scl_, true);
      Drive(sda_, true);
      scl_.port->OTYPER = scl_.port->OTYPER | scl_.mask;
      sda_.port->OTYPER = sda_.port->OTYPER | sda_.mask;
      Mode(scl_, 1);
      Mode(sda_, 1);
      __DSB();
    }

    void Leave() {
      Drive(scl_, true);
      Drive(sda_, true);
      Mode(scl_, 2);
      Mode(sda_, 2);
      __DSB();
    }

    Pin scl_, sda_;
  };

  struct I2cConfig {
    // Unshifted 0x08..0x77 address, never a HAL-shifted address or R/W byte.
    hal::i2c::Address address;
  };

  // The board supplies a validated TIMINGR value and its effective SCL rate.
  // Pins must be open-drain with pull-ups; idle() samples both SCL and SDA.
  class I2cBus {
   public:
    using Action = hal::i2c::Action;
    using Config = I2cConfig;

    constexpr I2cBus(I2C_TypeDef* registers, IRQn_Type event_irq,
                     IRQn_Type error_irq, std::uint32_t timing_register,
                     std::uint32_t effective_hz, const BusHardware& hardware,
                     const hal::i2c::BusClearPins& recovery = {})
        : registers_(registers),
          event_irq_(event_irq),
          error_irq_(error_irq),
          timing_register_(timing_register),
          effective_hz_(effective_hz),
          hardware_(hardware),
          recovery_(recovery) {}

    const void* identity() const { return registers_; }

    hal::Status validate(std::span<const Config> devices) const {
      invalid_device_.reset();
      const bool instance = (registers_ == I2C1 && event_irq_ == I2C1_EV_IRQn &&
                             error_irq_ == I2C1_ER_IRQn) ||
                            (registers_ == I2C2 && event_irq_ == I2C2_EV_IRQn &&
                             error_irq_ == I2C2_ER_IRQn) ||
                            (registers_ == I2C3 && event_irq_ == I2C3_EV_IRQn &&
                             error_irq_ == I2C3_ER_IRQn) ||
                            (registers_ == I2C4 && event_irq_ == I2C4_EV_IRQn &&
                             error_irq_ == I2C4_ER_IRQn);
      if (!instance || !effective_hz_ || !hardware_.prepare ||
          !hardware_.reset || !hardware_.idle || devices.empty()) {
        return hal::Status::invalid_argument;
      }
      for (std::size_t i = 0; i < devices.size(); ++i) {
        invalid_device_ = i;
        if (!devices[i].address.valid()) {
          return hal::Status::invalid_argument;
        }
        for (std::size_t j = 0; j < i; ++j) {
          if (devices[i].address.value == devices[j].address.value) {
            return hal::Status::invalid_argument;
          }
        }
      }
      invalid_device_.reset();
      return hal::Status::ok;
    }

    std::optional<std::size_t> invalid_device() const {
      return invalid_device_;
    }

    hal::Status init(std::span<const Config> devices) {
      devices_ = devices;
      prepared_ = true;
      const auto status = hardware_.prepare(hardware_.context);
      if (status != hal::Status::ok) {
        return status;
      }
      const auto reset_status = reset();
      if (reset_status != hal::Status::ok && !recovery_.available()) {
        return reset_status;
      }
      for (const auto irq : {event_irq_, error_irq_}) {
        NVIC_SetPriority(irq, (1u << __NVIC_PRIO_BITS) - 1);
        NVIC_ClearPendingIRQ(irq);
        NVIC_EnableIRQ(irq);
      }
      return hal::Status::ok;
    }

    void disable() {
      NVIC_DisableIRQ(event_irq_);
      NVIC_DisableIRQ(error_irq_);
      if (prepared_) {
        registers_->CR1 = 0;
        hardware_.reset(hardware_.context);
      }
      active_ = false;
      action_ = {};
    }

    std::uint32_t rate(std::size_t) const { return effective_hz_; }

    hal::Status validate_action(const Action&) const { return hal::Status::ok; }

    hal::Status begin(std::size_t device, std::span<const Action>) {
      address_ = std::uint32_t{devices_[device].address.value} << 1;
      return hardware_.idle(hardware_.context) ? hal::Status::ok
                                               : hal::Status::hardware_error;
    }

    hal::Status begin_probe(hal::i2c::Address address) {
      address_ = std::uint32_t{address.value} << 1;
      return hardware_.idle(hardware_.context) ? hal::Status::ok
                                               : hal::Status::hardware_error;
    }

    bool needs_reset() const { return !hardware_.idle(hardware_.context); }

    void start_reset() {
      (void)reset();
      registers_->CR1 = 0;
      recovery_.start();
    }

    std::optional<hal::Status> poll_reset(std::uint64_t now) {
      if (!recovery_.available()) {
        return reset();
      }
      const auto result = recovery_.poll(now);
      if (!result) {
        return std::nullopt;
      }
      const auto peripheral = reset();
      return *result == hal::Status::ok ? peripheral : *result;
    }

    void abort_reset() {
      recovery_.abort();
      (void)reset();
    }

    hal::Status start(const Action& action, bool, bool last) {
      action_ = action;
      offset_ = 0;
      total_ = std::max(action.tx.size(), action.rx.size());
      last_ = last;
      active_ = true;
      registers_->CR1 = I2C_CR1_PE | I2C_CR1_TXIE | I2C_CR1_RXIE |
                        I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE |
                        I2C_CR1_ERRIE;
      // Every action starts a fresh addressed phase, regardless of direction.
      Chunk(true);
      return hal::Status::ok;
    }

    std::optional<hal::Status> poll() {
      if (!active_) {
        return std::nullopt;
      }
      const auto flags = registers_->ISR;
      if (flags & (I2C_ISR_NACKF | I2C_ISR_ARLO | I2C_ISR_BERR | I2C_ISR_OVR)) {
        active_ = false;
        return flags & I2C_ISR_ARLO    ? hal::Status::arbitration_lost
               : flags & I2C_ISR_NACKF ? hal::Status::nack
                                       : hal::Status::hardware_error;
      }
      if ((flags & I2C_ISR_TXIS) && offset_ < chunk_end_ &&
          !action_.tx.empty()) {
        registers_->TXDR = action_.tx[offset_++];
      }
      if ((flags & I2C_ISR_RXNE) && offset_ < chunk_end_ &&
          !action_.rx.empty()) {
        action_.rx[offset_++] = static_cast<std::uint8_t>(registers_->RXDR);
      }
      if ((flags & I2C_ISR_TCR) && offset_ == chunk_end_) {
        Chunk(false);
      }
      if ((flags & I2C_ISR_STOPF) || ((flags & I2C_ISR_TC) && !last_)) {
        registers_->CR1 = I2C_CR1_PE;
        registers_->ICR = I2C_ICR_STOPCF;
        active_ = false;
        return offset_ == total_ ? hal::Status::ok
                                 : hal::Status::hardware_error;
      }
      return std::nullopt;
    }

    hal::Status finish() { return reset(); }

    hal::Status reset() {
      registers_->CR1 = 0;
      hardware_.reset(hardware_.context);
      active_ = false;
      action_ = {};
      registers_->TIMINGR = timing_register_;
      registers_->ICR = 0xffffffffu;
      registers_->CR1 = I2C_CR1_PE;
      NVIC_ClearPendingIRQ(event_irq_);
      NVIC_ClearPendingIRQ(error_irq_);
      return hardware_.idle(hardware_.context) ? hal::Status::ok
                                               : hal::Status::faulted;
    }

    void pend() { NVIC_SetPendingIRQ(event_irq_); }

   private:
    void Chunk(bool start) {
      // A standalone probe uses NBYTES=0 in write direction: AUTOEND
      // generates STOP after the address ACK/NACK, without a data byte.
      const auto count = std::min<std::size_t>(total_ - offset_, 255);
      chunk_end_ = offset_ + count;
      registers_->CR2 =
          address_ | (hal::detail::Reads(action_) ? I2C_CR2_RD_WRN : 0) |
          (static_cast<std::uint32_t>(count) << I2C_CR2_NBYTES_Pos) |
          (chunk_end_ < total_ ? I2C_CR2_RELOAD
           : last_             ? I2C_CR2_AUTOEND
                               : 0) |
          (start ? I2C_CR2_START : 0);
    }

    I2C_TypeDef* registers_;
    IRQn_Type event_irq_, error_irq_;
    std::uint32_t timing_register_, effective_hz_;
    BusHardware hardware_;
    hal::i2c::BusClear recovery_;
    mutable std::optional<std::size_t> invalid_device_;
    std::span<const Config> devices_;
    Action action_{};
    std::size_t offset_ = 0, total_ = 0, chunk_end_ = 0;
    std::uint32_t address_ = 0;
    bool last_ = false, active_ = false, prepared_ = false;
  };


}  // namespace daveos::platform::stm32::detail
