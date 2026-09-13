#pragma once

#include "daveos/platform/detail/synchronized.h"

namespace daveos::platform::fake {


// Deterministic host adapter with a clock initially at zero and no timer
// thread. advance()/automatic idle dispatch due timers synchronously through
// the shared simulated interrupt domain. External host threads may also inject
// interrupts.
class Platform final : public detail::Synchronized<Platform> {
 public:
  // automatic jumps to a finite idle deadline; manual waits for external
  // advance. In either mode, indefinite idle requires an external
  // notification/interrupt.
  enum class Advancement { manual, automatic };
  explicit Platform(Advancement mode = Advancement::automatic) : mode_(mode) {}
  ~Platform() { quiesce(); }
  // Current simulated microseconds; reading the clock never advances it.
  core::Time now() const { return time_.load(); }
  // Replace the pending timer. Expiry waits for time advancement, even at delay
  // 0.
  void arm(core::Time delay, Callback callback, void* context);
  // Cancel the pending callback without undoing one already selected for
  // dispatch.
  void disarm();
  // Permanently close timers/interrupt injection and wake idle waiters;
  // idempotent.
  void quiesce();
  // Scheduler hook: reject stale observations, then advance or wait by mode.
  void idle(core::Time deadline, bool sleep, std::uint64_t observed);
  // Advance by microseconds and dispatch every timer due along the way. Timers
  // can rearm themselves. Inside an ISR only move time; defer nested expiries
  // until a surrounding or later non-ISR advancement can dispatch them.
  void advance(core::Time amount);
  // Counts idle-path requests, including calls that return without waiting.
  std::uint64_t sleeps() const { return sleeps_.load(); }
  std::uint64_t awake_waits() const { return awake_waits_.load(); }

 private:
  void AdvanceTo(core::Time target);
  Advancement mode_;
  std::atomic<core::Time> time_{0};
  std::atomic<std::uint64_t> sleeps_{0}, awake_waits_{0};
  std::recursive_mutex advance_mutex_;
  std::mutex timer_mutex_;
  core::Time due_ = core::kForever;
  Callback callback_ = nullptr;
  void* argument_ = nullptr;
  bool closed_ = false;
};


}  // namespace daveos::platform::fake
