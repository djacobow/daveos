#include "platform/host/platform.h"

namespace daveos::platform::host {
Platform::Platform() : timer_thread_([this] { TimerLoop(); }) {}
Platform::~Platform() { quiesce(); }
core::Time Platform::now() const {
  return static_cast<core::Time>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - epoch_)
          .count());
}
void Platform::arm(core::Time delay, Callback callback, void* context) {
  {
    std::lock_guard lock(timer_mutex_);
    if (quit_) return;
    due_ = core::After(now(), delay);
    callback_ = callback;
    argument_ = context;
    ++generation_;
  }
  timer_cv_.notify_all();
}
void Platform::disarm() {
  {
    std::lock_guard lock(timer_mutex_);
    due_ = core::kForever;
    callback_ = nullptr;
    ++generation_;
  }
  timer_cv_.notify_all();
}
void Platform::quiesce() {
  {
    std::lock_guard lock(timer_mutex_);
    quit_ = true;
    callback_ = nullptr;
    due_ = core::kForever;
    ++generation_;
  }
  timer_cv_.notify_all();
  if (timer_thread_.joinable()) timer_thread_.join();
  CloseInterrupts();
  notify();
}
void Platform::TimerLoop() {
  std::unique_lock lock(timer_mutex_);
  while (!quit_) {
    auto generation = generation_;
    if (due_ == core::kForever) {
      timer_cv_.wait(lock, [&] { return quit_ || generation_ != generation; });
      continue;
    }
    // Clamp long waits to avoid overflowing the host clock's signed duration.
    auto current = now();
    auto delay =
        std::min<core::Time>(due_ > current ? due_ - current : 0, 1000000000);
    if (timer_cv_.wait_for(lock, std::chrono::microseconds(delay),
                           [&] { return quit_ || generation_ != generation; }))
      continue;
    if (now() < due_) continue;
    auto callback = callback_;
    auto argument = argument_;
    due_ = core::kForever;
    callback_ = nullptr;
    lock.unlock();
    if (callback) interrupt(callback, argument);
    lock.lock();
  }
}
void Platform::idle(core::Time deadline, bool /*sleep*/,
                    std::uint64_t observed) {
  // Host blocking is not an MCU power state; both paths use retained
  // notifications.
  std::unique_lock lock(wait_mutex_);
  auto changed = [&] { return sequence() != observed; };
  if (deadline == core::kForever) {
    wait_cv_.wait(lock, changed);
  } else {
    auto current = now();
    if (deadline > current) {
      auto delay = std::min<core::Time>(deadline - current, 1000000000);
      wait_cv_.wait_for(lock, std::chrono::microseconds(delay), changed);
    }
  }
}
}  // namespace daveos::platform::host
