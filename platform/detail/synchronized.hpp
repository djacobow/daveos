#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "core/platform/platform.hpp"

namespace daveos::platform::detail {


// Shared host/fake synchronization and simulated interrupt domain.
// Interrupt handlers serialize with each other but may run concurrently with
// module callbacks. Scheduler state uses a recursive mutex; application
// callbacks run outside that mutex. All external producer threads must finish
// before the adapter is destroyed. This implementation uses host threads, not
// MCU masking.
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
  // Nestable critical sections protecting core state across host threads.
  void enter() { state_mutex_.lock(); }

  void leave() { state_mutex_.unlock(); }

  // Queue operations share a try-lock mutex distinct from scheduler state.
  QueueMutex* queue_mutex() { return &queue_mutex_; }

  // Context is thread-local; a concurrent module retains its own attribution.
  bool in_interrupt() const { return interrupt_; }

  core::Context context() const { return context_; }

  void context(core::Context value) { context_ = value; }

  // Retained generation, sampled before checking work and passed to idle().
  std::uint64_t sequence() const { return sequence_.load(); }

  // Advance the generation and wake waiters without losing an early
  // notification.
  void notify() {
    {
      std::lock_guard lock(wait_mutex_);
      ++sequence_;
    }
    wait_cv_.notify_all();
  }

  // Synchronous trigger: handlers from different host threads are serialized.
  // Null callbacks and nested injection return invalid_argument. Once closed,
  // injection returns not_running. Callback/context must survive this call.
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
  // Wait for the active handler, then reject new injections. Never call in an
  // ISR.
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
