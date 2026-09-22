#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "core/foundation/types.hpp"

namespace daveos::core {


  struct StateStatistics {
    std::uint64_t ticks = 0;
    std::uint64_t entries = 0;
  };

  // States must be contiguous enum-class values [0, Count). Derived implements
  // void Step(State cs, State& ns, tick arguments...): only its switch(cs)
  // may change ns. Optional void OnEnter/OnExit/OnTick(State, tick
  // arguments...) hooks cannot select transitions. Arguments are borrowed as
  // lvalues for this call only; the helper neither stores nor moves from them.
  // All access belongs to one execution context; this class is not
  // synchronized.
  template <typename Derived, typename State, State Initial, std::size_t Count>
  class StateMachine {
    static_assert(std::is_enum_v<State> && !std::is_convertible_v<State, int>,
                  "StateMachine requires an enum class");
    static_assert(Count > 0);
    static_assert(static_cast<std::uintmax_t>(Initial) < Count,
                  "Initial state must be in [0, Count)");

   public:
    using StatisticsSnapshot = std::array<StateStatistics, Count>;

    // Initial entry is deferred until this first call. Each tick is credited to
    // the departing state; a newly entered state has dwell zero until next
    // tick. Recursive ticks return busy. An invalid next state returns
    // invalid_argument without exit/entry or a state change (the current tick
    // is still counted).
    template <typename... Args>
    Status tick(Args&&... args) {
      State ns = cs;
      if (ticking_) {
        return Status::busy;
      }
      ticking_ = true;
      auto& derived = static_cast<Derived&>(*this);
      if (!started_) {
        started_ = true;
        Enter(derived, args...);
      }
      Increment(dwell_);
      Increment(statistics_[Index(cs)].ticks);
      if constexpr (requires { derived.OnTick(cs, args...); }) {
        static_assert(
            std::same_as<decltype(derived.OnTick(cs, args...)), void>);
        derived.OnTick(cs, args...);
      }
      static_assert(
          requires {
            { derived.Step(cs, ns, args...) } -> std::same_as<void>;
          },
          "StateMachine requires void Step(State, State&, tick arguments...)");
      derived.Step(cs, ns, args...);
      if (!Valid(ns)) {
        ticking_ = false;
        return Status::invalid_argument;
      }
      if (ns != cs) {
        if constexpr (requires { derived.OnExit(cs, args...); }) {
          static_assert(
              std::same_as<decltype(derived.OnExit(cs, args...)), void>);
          derived.OnExit(cs, args...);
        }
        cs = ns;
        dwell_ = 0;
        Enter(derived, args...);
      }
      ticking_ = false;
      return Status::ok;
    }

    State state() const { return cs; }

    std::uint64_t dwell_count() const { return dwell_; }

    // Unknown enum values return zero statistics. All getters return copies.
    StateStatistics statistics(State state) const {
      return Valid(state) ? statistics_[Index(state)] : StateStatistics{};
    }

    // An independent snapshot must not change when subsequent ticks run.
    // cppcheck-suppress returnByReference
    StatisticsSnapshot statistics() const { return statistics_; }

   protected:
    // No callbacks or derived-object access during construction.
    StateMachine() = default;

   private:
    static constexpr bool Valid(State state) {
      return static_cast<std::uintmax_t>(state) < Count;
    }

    static constexpr std::size_t Index(State state) {
      return static_cast<std::size_t>(state);
    }

    // Diagnostic counters saturate rather than wrap. Hooks/Step must not throw.
    static void Increment(std::uint64_t& count) {
      if (count != std::numeric_limits<std::uint64_t>::max()) {
        ++count;
      }
    }

    template <typename... Args>
    void Enter(Derived& derived, Args&... args) {
      Increment(statistics_[Index(cs)].entries);
      if constexpr (requires { derived.OnEnter(cs, args...); }) {
        static_assert(
            std::same_as<decltype(derived.OnEnter(cs, args...)), void>);
        derived.OnEnter(cs, args...);
      }
    }

    State cs = Initial;
    StatisticsSnapshot statistics_{};
    std::uint64_t dwell_ = 0;
    bool started_ = false;
    bool ticking_ = false;
  };


}  // namespace daveos::core
