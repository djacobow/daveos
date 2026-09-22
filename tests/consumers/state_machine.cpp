// A state machine alone: foundation types only, no platform or scheduler.
#include "core/state_machine/state_machine.hpp"

namespace {
  enum class State { idle, running };

  class Machine
      : public daveos::core::StateMachine<Machine, State, State::idle, 2> {
    friend class daveos::core::StateMachine<Machine, State, State::idle, 2>;

    void Step(State cs, State& ns) {
      if (cs == State::idle) {
        ns = State::running;
      }
    }
  };
}  // namespace

int main() {
  Machine machine;
  machine.tick();
  return machine.state() == State::running ? 0 : 1;
}
