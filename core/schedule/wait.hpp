#pragma once

#include "duration.hpp"

namespace daveos::core {


  // Wait until borrowed buffers are released, NOT merely until a deadline.
  // ready() must only become true once those buffers are safe to reuse. pump()
  // normally calls scheduler.yield(); tests can also advance their injected
  // clock there. now() is monotonic microseconds. No allocation, sleep or
  // cancellation.
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
  Status wait_for_release(Ready&& ready, Pump&& pump, Clock&& now,
                          std::chrono::duration<Rep, Period> timeout) {
    Time budget = 0;
    Status result = to_microseconds(timeout, budget);
    const Time started = now();
    while (!ready()) {
      if (result == Status::ok && now() - started >= budget) {
        result = Status::timeout;
      }
      const auto status = pump();
      if (result == Status::ok && status != Status::ok &&
          status != Status::empty && status != Status::depth_limit) {
        result = status;
      }
    }
    return result;
  }


}  // namespace daveos::core
