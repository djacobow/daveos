#include "core/state_machine/state_machine.hpp"

#include "support.hpp"

namespace core = daveos::core;

namespace {

  enum class State { idle, working, done };

  struct Machine : core::StateMachine<Machine, State, State::idle, 3> {
    struct Observation {
      char hook;
      State state;
      std::uint64_t dwell;
      std::uint64_t ticks;
      std::uint64_t entries;
    };

    std::vector<Observation> observations;
    bool work = false, finish = false, restart = false, invalid = false;
    bool recurse = false;
    core::Status recursive_status = core::Status::ok;

    void OnEnter(State value) { Observe('e', value); }

    void OnExit(State value) { Observe('x', value); }

    void OnTick(State value) {
      Observe('t', value);
      if (recurse) {
        recursive_status = tick();
      }
    }

    void Step(State cs, State& ns) {
      switch (cs) {
        case State::idle:
          if (invalid) {
            ns = static_cast<State>(-1);
          } else if (work) {
            ns = State::working;
          }
          break;
        case State::working:
          if (finish) {
            ns = State::done;
          }
          break;
        case State::done:
          if (restart) {
            ns = State::idle;
          }
          break;
      }
    }

    void Observe(char hook, State value) {
      REQUIRE(value == state());
      const auto stats = statistics(value);
      observations.push_back(
          {hook, value, dwell_count(), stats.ticks, stats.entries});
    }
  };

  // Private callbacks are supported through friendship; all hooks are optional.
  class Minimal : public core::StateMachine<Minimal, State, State::done, 3> {
    friend class core::StateMachine<Minimal, State, State::done, 3>;

    void Step(State cs, State& ns) {
      switch (cs) {
        case State::done:
          ns = State::idle;
          break;
        default:
          break;
      }
    }
  };

}  // namespace

TEST_CASE(
    "state machine defers entry and accounts ticks at transition boundaries") {
  Machine machine;
  REQUIRE(machine.state() == State::idle);
  REQUIRE(machine.dwell_count() == 0);
  REQUIRE(machine.statistics(State::idle).entries == 0);
  REQUIRE(machine.observations.empty());
  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.observations.size() == 2);
  REQUIRE(machine.observations[0].hook == 'e');
  REQUIRE(machine.observations[0].dwell == 0);
  REQUIRE(machine.observations[0].entries == 1);
  REQUIRE(machine.observations[0].ticks == 0);
  REQUIRE(machine.observations[1].hook == 't');
  REQUIRE(machine.observations[1].dwell == 1);
  REQUIRE(machine.observations[1].ticks == 1);

  machine.work = true;
  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.observations.size() == 5);
  REQUIRE(machine.observations[2].hook == 't');
  REQUIRE(machine.observations[3].hook == 'x');
  REQUIRE(machine.observations[3].state == State::idle);
  REQUIRE(machine.observations[3].dwell == 2);
  REQUIRE(machine.observations[4].hook == 'e');
  REQUIRE(machine.observations[4].state == State::working);
  REQUIRE(machine.observations[4].dwell == 0);
  REQUIRE(machine.observations[4].ticks == 0);
  REQUIRE(machine.state() == State::working);
  REQUIRE(machine.dwell_count() == 0);
  REQUIRE(machine.statistics(State::idle).ticks == 2);
  REQUIRE(machine.statistics(State::working).entries == 1);

  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.dwell_count() == 1);
  REQUIRE(machine.observations.size() ==
          6);  // Same-state ticks do not reenter.
  machine.finish = true;
  REQUIRE(machine.tick() == core::Status::ok);
  machine.restart = true;
  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.state() == State::idle);
  REQUIRE(machine.dwell_count() == 0);
  REQUIRE(machine.statistics(State::idle).ticks == 2);
  REQUIRE(machine.statistics(State::idle).entries == 2);
  REQUIRE(machine.statistics(State::working).ticks == 2);
  REQUIRE(machine.statistics(State::done).ticks == 1);
  auto snapshot = machine.statistics();
  snapshot[0].ticks = 999;
  REQUIRE(machine.statistics(State::idle).ticks == 2);
  REQUIRE(machine.statistics(static_cast<State>(-1)).ticks == 0);
  REQUIRE(machine.statistics(static_cast<State>(3)).entries == 0);
}

TEST_CASE(
    "state machine rejects recursion and invalid transitions without "
    "committing") {
  Machine machine;
  machine.recurse = true;
  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.recursive_status == core::Status::busy);
  REQUIRE(machine.dwell_count() == 1);
  machine.invalid = true;
  REQUIRE(machine.tick() == core::Status::invalid_argument);
  REQUIRE(machine.state() == State::idle);
  REQUIRE(machine.dwell_count() == 2);
  REQUIRE(machine.observations.size() == 3);
  machine.invalid = false;
  machine.work = true;
  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.state() == State::working);
}

TEST_CASE("state machine needs no hooks and supports a nonzero initial state") {
  Minimal machine;
  REQUIRE(machine.state() == State::done);
  REQUIRE(machine.tick() == core::Status::ok);
  REQUIRE(machine.state() == State::idle);
  REQUIRE(machine.statistics(State::done).ticks == 1);
  REQUIRE(machine.statistics(State::done).entries == 1);
  REQUIRE(machine.statistics(State::idle).ticks == 0);
  REQUIRE(machine.statistics(State::idle).entries == 1);
}

TEST_CASE(
    "tick borrows context and input through every hook without retaining "
    "them") {
  struct Context {
    Context() = default;
    Context(const Context&) = delete;
    std::vector<char> actions;
  };

  struct Borrowing : core::StateMachine<Borrowing, State, State::idle, 3> {
    void OnEnter(State, Context& context, std::span<const char>, std::size_t&) {
      context.actions.push_back('e');
    }

    void OnTick(State, Context& context, std::span<const char>, std::size_t&) {
      context.actions.push_back('t');
    }

    void OnExit(State, Context& context, std::span<const char>, std::size_t&) {
      context.actions.push_back('x');
    }

    void Step(State cs, State& ns, Context& context,
              std::span<const char> input, std::size_t& consumed) {
      context.actions.push_back('s');
      switch (cs) {
        case State::idle:
          if (!input.empty()) {
            ++consumed;
            ns = State::working;
          }
          break;
        default:
          break;
      }
    }
  } machine;

  Context first;
  std::size_t consumed = 0;
  REQUIRE(machine.tick(first, std::span<const char>("a", 1), consumed) ==
          core::Status::ok);
  REQUIRE(first.actions == std::vector<char>{'e', 't', 's', 'x', 'e'});
  REQUIRE(consumed == 1);
  REQUIRE(machine.state() == State::working);
  Context second;
  REQUIRE(machine.tick(second, std::span<const char>{}, consumed) ==
          core::Status::ok);
  REQUIRE(second.actions == std::vector<char>{'t', 's'});
  REQUIRE(first.actions.size() == 5);
  REQUIRE(machine.dwell_count() == 1);
}
