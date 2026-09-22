#pragma once

#include <chrono>

#include "core/foundation/types.hpp"
#include "core/state_machine/state_machine.hpp"

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
      (void)machine_.tick(*this, now, healthy);
      return status_;
    }

    State state() const { return machine_.state(); }

    std::uint64_t dwell_count() const { return machine_.dwell_count(); }

    core::StateStatistics statistics(State state) const {
      return machine_.statistics(state);
    }

    auto statistics() const { return machine_.statistics(); }

   private:
    class Machine
        : public core::StateMachine<Machine, State, State::waiting,
                                    static_cast<std::size_t>(State::failed) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::waiting,
                                      static_cast<std::size_t>(State::failed) +
                                          1>;

      void Step(State cs, State& ns, Confirmation& owner, core::Time now,
                bool healthy) {
        owner.Step(cs, ns, now, healthy);
      }
    };

    void Step(State cs, State& ns, core::Time now, bool healthy) {
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
    }

    std::chrono::microseconds delay_;
    Machine machine_;
    core::Time since_ = 0;
    core::Status status_ = core::Status::ok;
    void* context_ = nullptr;
    core::Status (*confirm_)(void*) = nullptr;
  };


}  // namespace daveos::watchdog
