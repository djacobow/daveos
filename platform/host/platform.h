#pragma once

#include <chrono>
#include <thread>

#include "platform/detail/synchronized.hpp"

namespace daveos::platform::host {


// Real-time host adapter with a steady clock and one dedicated timer thread.
// Timer expiry enters Synchronized's simulated interrupt domain; module
// callbacks may run concurrently. Owns host synchronization resources and is
// single-use.
class Platform final : public detail::Synchronized<Platform> {
 public:
  // Start the timer thread; no callback is armed initially.
  Platform();
  ~Platform();
  // Microseconds since construction, independent of wall-clock changes.
  core::Time now() const;
  // Replace the one pending timer. Callback runs asynchronously even for zero
  // delay; its context must survive dispatch/cancellation and any active
  // callback.
  void arm(core::Time delay, Callback callback, void* context);
  // Cancel pending expiry; does not wait for an already selected callback.
  void disarm();
  // Permanently stop/join the timer thread and close interrupt injection.
  // Idempotent; must be called outside interrupt context (also used by
  // destructor).
  void quiesce();
  // Wait for the absolute deadline or a generation change; kForever waits
  // indefinitely. Both sleep/awake paths use a host condition variable.
  void idle(core::Time deadline, bool sleep, std::uint64_t observed);

 private:
  void TimerLoop();
  const std::chrono::steady_clock::time_point epoch_ =
      std::chrono::steady_clock::now();
  std::mutex timer_mutex_;
  std::condition_variable timer_cv_;
  core::Time due_ = core::kForever;
  Callback callback_ = nullptr;
  void* argument_ = nullptr;
  std::uint64_t generation_ = 0;
  bool quit_ = false;
  std::thread timer_thread_;
};


}  // namespace daveos::platform::host
