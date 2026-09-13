#pragma once

#include <chrono>
#include <thread>

#include "daveos/platform/detail/synchronized.h"

namespace daveos::platform::host {
class Platform final : public detail::Synchronized<Platform> {
 public:
  Platform();
  ~Platform();
  core::Time now() const;
  void arm(core::Time delay, Callback callback, void* context);
  void disarm();
  void quiesce();
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
