#pragma once

#include "core/platform/platform.hpp"

namespace daveos::platform::detail {


// Owns TIM2 and channel 1. The application forwards TIM2_IRQHandler to
// interrupt(), and supplies the timer kernel frequency after clock setup.
// Reserve this peripheral exclusively for the adapter. The timer clock must
// remain constant while initialized. TIM2 continues in shallow sleep; deep
// sleep/Stop modes are unsupported. SysTick remains available to the HAL.
// Critical sections mask configurable interrupts: NMI/HardFault must not call
// DaveOS. Run the scheduler in thread mode with interrupts enabled.
template <typename Derived>
class Stm32Tim2 : public core::Platform<Derived> {
 public:
  using Callback = typename core::Platform<Derived>::Callback;
  Stm32Tim2() = default;

  ~Stm32Tim2() { quiesce(); }

  Stm32Tim2(const Stm32Tim2&) = delete;
  Stm32Tim2& operator=(const Stm32Tim2&) = delete;
  // Start a 1 MHz counter after clock setup. timer_hz must divide exactly by
  // 1 MHz and fit the 16-bit prescaler. Repeated init returns
  // already_initialized.
  core::Status init(std::uint32_t timer_hz);
  // Request a CMSIS system reset (both cores on H755). Does not return.
  // No initialization prerequisite or graceful shutdown/log drain.
  [[noreturn]] core::Status reset();
  // Extended 64-bit microseconds since init. Rollover is serviced by the IRQ or
  // this read; interrupts must not stay masked for a full 32-bit counter
  // period.
  core::Time now();
  // Nestable PRIMASK save/restore, also used as the queue mutex fallback.
  void enter();
  void leave();
  // Replace the pending callback; delay zero pends the IRQ instead of calling
  // inline. Large delays use intermediate compares. Requires successful init().
  void arm(core::Time delay, Callback callback, void* argument);
  // Cancel compare expiry, retaining the overflow interrupt for timekeeping.
  void disarm();
  // Disable the timer/IRQ and discard the callback; safe to repeat outside an
  // ISR.
  void quiesce();
  // TIM2 IRQ entry point. Services rollover and invokes any due callback once.
  void interrupt();

  // Embedded run() is intended to continue indefinitely.
  bool can_stop() const { return false; }

  bool in_interrupt() const;
  // ISR attribution is always core/interrupt; thread context is saved
  // separately.
  core::Context context() const;
  void context(core::Context value);
  // Retained work generation, synchronized by the same critical section guard.
  std::uint64_t sequence();
  void notify();
  // Check deadline/generation atomically before shallow WFI, or return to poll
  // when sleep is false. The scheduler must have armed its next wake deadline.
  void idle(core::Time deadline, bool sleep, std::uint64_t observed);

 private:
  void ProgramCompare();
  bool initialized_ = false;
  std::uint32_t depth_ = 0, saved_mask_ = 0;
  core::Time epoch_ = 0, due_ = core::kForever;
  std::uint64_t sequence_ = 0;
  Callback callback_ = nullptr;
  void* argument_ = nullptr;
  core::Context context_{};
};


}  // namespace daveos::platform::detail
