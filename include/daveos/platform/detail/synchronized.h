#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "daveos/core/platform.h"

namespace daveos::platform::detail {
// Shared host/fake interrupt domain. Application callbacks never hold
// state_mutex_.
template <typename Derived>
class Synchronized : public core::Platform<Derived> {
 public:
  using Callback = typename core::Platform<Derived>::Callback;

 private:
  class QueueMutex final {
   public:
    bool try_lock() { return mutex_.try_lock(); }
    void unlock() { mutex_.unlock(); }

   private:
    std::mutex mutex_;
  };

 public:
  void enter() { state_mutex_.lock(); }
  void leave() { state_mutex_.unlock(); }
  QueueMutex* queue_mutex() { return &queue_mutex_; }
  bool in_interrupt() const { return interrupt_; }
  core::Context context() const { return context_; }
  void context(core::Context value) { context_ = value; }
  std::uint64_t sequence() const { return sequence_.load(); }
  void notify() {
    {
      std::lock_guard lock(wait_mutex_);
      ++sequence_;
    }
    wait_cv_.notify_all();
  }
  // Synchronous trigger: handlers from different host threads are serialized.
  core::Status interrupt(Callback callback, void* argument = nullptr) {
    if (!callback || interrupt_) return core::Status::invalid_argument;
    std::lock_guard serial(interrupt_mutex_);
    {
      core::Guard guard(*this);
      if (closed_) return core::Status::not_running;
    }
    core::ContextGuard context(*this, {"core", "interrupt"});
    interrupt_ = true;
    callback(argument);
    interrupt_ = false;
    notify();
    return core::Status::ok;
  }

 protected:
  void CloseInterrupts() {
    std::lock_guard serial(interrupt_mutex_);
    core::Guard guard(*this);
    closed_ = true;
  }
  mutable std::mutex wait_mutex_;
  std::condition_variable wait_cv_;

 private:
  std::recursive_mutex state_mutex_;
  std::mutex interrupt_mutex_;
  QueueMutex queue_mutex_;
  std::atomic<std::uint64_t> sequence_{0};
  bool closed_ = false;
  inline static thread_local bool interrupt_ = false;
  inline static thread_local core::Context context_{};
};
}  // namespace daveos::platform::detail
