#pragma once

#include <chrono>

#include "core/platform/platform.hpp"

namespace daveos::watchdog {


  // Optional application policy: one confirmation attempt after an
  // uninterrupted healthy interval. A failed check or confirmation permanently
  // closes the gate.
  class Confirmation {
   public:
    enum class State { waiting, observing, confirmed, failed };

    explicit Confirmation(std::chrono::microseconds delay) : delay_(delay) {}

    // Configure before the first tick; keep the callback alive through run().
    void callback(void* context, core::Status (*confirm)(void*)) {
      context_ = context;
      confirm_ = confirm;
    }

    core::Status tick(core::Time now, bool healthy) {
      State ns = cs;
      switch (cs) {
        case State::waiting:
          if (!healthy || delay_.count() <= 0) {
            status_ = core::Status::health_failed;
            ns = State::failed;
          } else if (confirm_) {
            since_ = now;
            ns = State::observing;
          }
          break;
        case State::observing:
          if (!healthy || now < since_) {
            status_ = core::Status::health_failed;
            ns = State::failed;
          } else if (now - since_ >= static_cast<core::Time>(delay_.count())) {
            status_ = confirm_(context_);
            ns = status_ == core::Status::ok ? State::confirmed : State::failed;
          }
          break;
        case State::confirmed:
        case State::failed:
          break;
      }
      if (ns != cs) {
        cs = ns;
      }
      return status_;
    }

    State state() const { return cs; }

   private:
    std::chrono::microseconds delay_;
    State cs = State::waiting;
    core::Time since_ = 0;
    core::Status status_ = core::Status::ok;
    void* context_ = nullptr;
    core::Status (*confirm_)(void*) = nullptr;
  };


}  // namespace daveos::watchdog
