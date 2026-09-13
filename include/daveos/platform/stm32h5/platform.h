#pragma once

#include "daveos/core/platform.h"

namespace daveos::platform::stm32h5 {
// Owns TIM2 and channel 1. The application forwards TIM2_IRQHandler to
// interrupt(), and supplies the timer kernel frequency after clock setup.
class Platform final : public core::Platform<Platform> {
 public:
  Platform() = default;
  ~Platform() { quiesce(); }
  Platform(const Platform&) = delete;
  Platform& operator=(const Platform&) = delete;
  core::Status init(std::uint32_t timer_hz);
  core::Time now();
  void enter();
  void leave();
  void arm(core::Time delay, Callback callback, void* argument);
  void disarm();
  void quiesce();
  void interrupt();
  bool can_stop() const { return false; }
  bool in_interrupt() const;
  core::Context context() const;
  void context(core::Context value);
  std::uint64_t sequence();
  void notify();
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
}  // namespace daveos::platform::stm32h5
