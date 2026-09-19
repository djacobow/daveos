#include "platform/fake/platform.h"

namespace daveos::platform::fake {
  void Platform::arm(core::Time delay, Callback callback, void* context) {
    std::lock_guard lock(timer_mutex_);
    if (closed_) {
      return;
    }
    due_ = core::After(now(), delay);
    callback_ = callback;
    argument_ = context;
  }

  void Platform::disarm() {
    std::lock_guard lock(timer_mutex_);
    due_ = core::kForever;
    callback_ = nullptr;
  }

  void Platform::quiesce() {
    {
      std::lock_guard lock(timer_mutex_);
      closed_ = true;
      due_ = core::kForever;
      callback_ = nullptr;
    }
    CloseInterrupts();
    notify();
  }

  void Platform::advance(core::Time amount) {
    std::lock_guard lock(advance_mutex_);
    if (in_interrupt()) {
      // Time may elapse inside an ISR, but a second ISR cannot nest in the fake
      // domain.
      time_ = core::After(now(), amount);
      notify();
      return;
    }
    AdvanceTo(core::After(now(), amount));
  }

  void Platform::AdvanceTo(core::Time target) {
    while (true) {
      Callback callback = nullptr;
      void* argument = nullptr;
      {
        std::lock_guard lock(timer_mutex_);
        auto limit = target > now() ? target : now();
        if (due_ > limit || !callback_ || closed_) {
          break;
        }
        if (due_ > now()) {
          time_ = due_;
        }
        callback = callback_;
        argument = argument_;
        callback_ = nullptr;
        due_ = core::kForever;
      }
      interrupt(callback, argument);
    }
    if (target > now()) {
      time_ = target;
    }
    notify();
  }

  void Platform::idle(core::Time deadline, bool sleep, std::uint64_t observed) {
    if (sleep) {
      ++sleeps_;
    } else {
      ++awake_waits_;
    }
    if (sequence() != observed) {
      return;
    }
    if (mode_ == Advancement::automatic && deadline != core::kForever) {
      std::lock_guard lock(advance_mutex_);
      AdvanceTo(deadline);
      return;
    }
    std::unique_lock lock(wait_mutex_);
    wait_cv_.wait(lock,
                  [&] { return sequence() != observed || now() >= deadline; });
  }
}  // namespace daveos::platform::fake
