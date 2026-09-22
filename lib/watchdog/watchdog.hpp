#pragma once

#include <span>

#include "core/schedule/module.hpp"
#include "core/schedule/progress.h"
#include "core/state_machine/state_machine.hpp"
#include "driver.h"

namespace daveos::watchdog {


  namespace core = daveos::core;

  // Borrowed names are consumed synchronously by the failure observer. A
  // retained diagnostic backend must copy them, not store these pointers.
  struct Failure {
    core::Status status = core::Status::ok;
    const char* check = "";
    const char* module = "";
    const char* task = "";
    std::uint64_t expected = 0;
    std::uint64_t completed = 0;
  };

  struct Check {
    const char* name;
    void* context;
    Failure (*test)(void*);
  };

  struct Observer {
    void* context = nullptr;
    void (*failed)(void*, const Failure&) = nullptr;
  };

  // A missed iteration is counted only when its deadline is strictly older
  // than the allowance. This handles phase, slow tasks and the check's own
  // still-running iteration without special-casing any module.
  template <std::size_t Tasks>
  Failure check_progress(const core::Progress<Tasks>& progress,
                         core::Time allowance) {
    if (!progress.running || progress.now <= allowance) {
      return {};
    }
    const auto cutoff = progress.now - allowance;
    for (const auto& task : progress.tasks) {
      if (!task.repeating || !task.period || task.first_due >= cutoff) {
        continue;
      }
      const auto expected = (cutoff - 1 - task.first_due) / task.period + 1;
      if (task.completed < expected) {
        return {core::Status::health_failed,
                "task_progress",
                task.module,
                task.task,
                expected,
                task.completed};
      }
    }
    return {};
  }

  // No construction-time hardware access. start() may precede module init;
  // after start, only scheduled calls to tick() feed. Failures never recover.
  class Controller {
   public:
    enum class State { stopped, running, failed };

    Controller(Driver driver, std::span<const Check> checks,
               Observer observer = {})
        : driver_(driver), checks_(checks), observer_(observer) {}

    core::Status start(core::Time timeout) {
      if (state() != State::stopped) {
        return state() == State::failed ? failure_.status
                                        : core::Status::already_initialized;
      }
      if (!timeout || timeout == core::kForever || !driver_.start ||
          !driver_.feed) {
        return core::Status::invalid_argument;
      }
      timeout_ = timeout;
      start_requested_ = true;
      tick();
      return failure_.status;
    }

    template <core::DurationRep Rep, typename Period>
    core::Status start(std::chrono::duration<Rep, Period> timeout) {
      core::Time micros = 0;
      auto status = core::to_microseconds(timeout, micros);
      return status == core::Status::ok ? start(micros) : status;
    }

    void tick() { (void)machine_.tick(*this); }

    State state() const { return machine_.state(); }

    std::uint64_t dwell_count() const { return machine_.dwell_count(); }

    core::StateStatistics statistics(State state) const {
      return machine_.statistics(state);
    }

    auto statistics() const { return machine_.statistics(); }

    const Failure& failure() const { return failure_; }

   private:
    class Machine
        : public core::StateMachine<Machine, State, State::stopped,
                                    static_cast<std::size_t>(State::failed) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::stopped,
                                      static_cast<std::size_t>(State::failed) +
                                          1>;

      void Step(State cs, State& ns, Controller& owner) { owner.Step(cs, ns); }

      void OnEnter(State state, Controller& owner) {
        if (state == State::failed && owner.observer_.failed) {
          owner.observer_.failed(owner.observer_.context, owner.failure_);
        }
      }
    };

    void Step(State cs, State& ns) {
      switch (cs) {
        case State::stopped: {
          if (start_requested_) {
            start_requested_ = false;
            failure_ = CheckHealth();
            if (failure_.status == core::Status::ok) {
              failure_ = {driver_.start(driver_.context, timeout_), "start"};
            }
            ns = failure_.status == core::Status::ok ? State::running
                                                     : State::failed;
          }
          break;
        }
        case State::running: {
          failure_ = CheckHealth();
          if (failure_.status == core::Status::ok) {
            failure_ = {driver_.feed(driver_.context), "feed"};
          }
          if (failure_.status != core::Status::ok) {
            ns = State::failed;
          }
          break;
        }
        case State::failed: {
          break;
        }
      }
    }

    Failure CheckHealth() const {
      for (const auto& check : checks_) {
        auto result = check.test ? check.test(check.context)
                                 : Failure{core::Status::invalid_argument};
        if (result.status != core::Status::ok) {
          result.check = check.name;
          return result;
        }
      }
      return {};
    }

    Driver driver_;
    std::span<const Check> checks_;
    Observer observer_;
    Failure failure_{};
    Machine machine_;
    core::Time timeout_ = 0;
    bool start_requested_ = false;
  };

  // Explicit start is independent of stage1/stage2. Module init only schedules
  // checks; it must not arm or feed hardware during application initialization.
  template <typename Event = core::NoEvent>
  class Module : public core::Module<Module<Event>, Event> {
   public:
    Module(Controller& controller, std::chrono::microseconds period)
        : controller_(controller), period_(period) {}

    static constexpr const char* name() { return "watchdog"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Module, Poll)};
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage1
                 ? this->template schedule<&Module::Poll>(period_,
                                                          core::Mode::repeat)
                 : core::Status::ok;
    }

    void Poll() { controller_.tick(); }

   private:
    Controller& controller_;
    std::chrono::microseconds period_;
  };


}  // namespace daveos::watchdog
