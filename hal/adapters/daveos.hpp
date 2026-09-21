#pragma once

#include "core/platform/platform.hpp"
#include "core/platform/timer_callback.hpp"
#include "hal/status.h"

namespace daveos::hal {


  // Scheduler must be running before submission. Selected callbacks may still
  // execute after cancellation: Controller reevaluates current deadlines on
  // every notification. All bus IRQs and timer callbacks must be serialized
  // (same preemption priority on STM32, platform.interrupt() on host).
  template <typename Scheduler, typename Platform>
  class DaveOsClock {
   public:
    constexpr DaveOsClock(Scheduler& scheduler, Platform& platform)
        : scheduler_(&scheduler), platform_(platform) {}

    explicit constexpr DaveOsClock(Platform& platform) : platform_(platform) {}

    void bind(Scheduler& scheduler) { scheduler_ = &scheduler; }

    std::uint64_t now() const { return platform_.now(); }

    Status arm(std::uint64_t deadline, const core::TimerCallback& callback) {
      if (!scheduler_) {
        return Status::timer_error;
      }
      const auto time = now();
      if (deadline <= time) {
        return Status::timer_error;
      }
      return scheduler_->timer(deadline - time, callback) == core::Status::ok
                 ? Status::ok
                 : Status::timer_error;
    }

    void cancel(const core::TimerCallback& callback) {
      if (scheduler_) {
        scheduler_->cancel_timer(callback);
      }
    }

   private:
    Scheduler* scheduler_ = nullptr;
    Platform& platform_;
  };


}  // namespace daveos::hal
