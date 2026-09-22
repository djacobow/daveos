#pragma once

#include <functional>

#include "duration.hpp"

namespace daveos::core {


  // Wait for completion/ownership release, NOT merely a deadline. ready() must
  // only become true once borrowed buffers are safe to reuse. pump() normally
  // calls scheduler.yield(); tests can also advance their injected clock there.
  // now() is monotonic microseconds. No allocation, sleep or cancellation.
  //
  // The first deadline/argument/pump failure is retained, but pumping continues
  // until ready(). ok, empty and depth_limit are normal yield outcomes. Even
  // stop/invalid context cannot release live buffers. The backend must provide
  // bounded completion independently of nested tasks (usually via interrupts).
  // A broken backend can therefore block forever; this is NOT a bounded wait.
  // An already-ready operation wins over the deadline. Check its own result
  // separately: this return value describes waiting, not peripheral success.
  template <typename Ready, typename Pump, typename Clock, DurationRep Rep,
            typename Period>
  Status wait_until(Ready&& ready, Pump&& pump, Clock&& now,
                    std::chrono::duration<Rep, Period> timeout) {
    Time budget = 0;
    Status result = to_microseconds(timeout, budget);
    const Time started = std::invoke(now);
    while (!std::invoke(ready)) {
      if (result == Status::ok && std::invoke(now) - started >= budget) {
        result = Status::timeout;
      }
      const auto status = std::invoke(pump);
      if (result == Status::ok && status != Status::ok &&
          status != Status::empty && status != Status::depth_limit) {
        result = status;
      }
    }
    return result;
  }


}  // namespace daveos::core
