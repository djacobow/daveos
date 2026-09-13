#pragma once

#include "daveos/platform/detail/synchronized.h"

namespace daveos::platform::fake {
class Platform final : public detail::Synchronized<Platform> {
 public:
  enum class Advancement { manual, automatic };
  explicit Platform(Advancement mode = Advancement::automatic) : mode_(mode) {}
  ~Platform() { quiesce(); }
  core::Time now() const { return time_.load(); }
  void arm(core::Time delay, Callback callback, void* context);
  void disarm();
  void quiesce();
  void idle(core::Time deadline, bool sleep, std::uint64_t observed);
  void advance(core::Time amount);
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
