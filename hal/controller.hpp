#pragma once

#include <array>
#include <atomic>

#include "core/platform/timer_callback.hpp"
#include "core/state_machine/state_machine.hpp"
#include "hal/detail/actions.hpp"
#include "hal/detail/counters.hpp"

namespace daveos::hal {


  // One instance per physical controller. Backend owns register/IRQ details:
  // validate(configs), init(configs), rate(device), validate_action(action),
  // begin(device, actions), start(action, first, last), poll(), finish(),
  // reset(), disable(), pend(). poll returns an optional completed-action
  // status. finish/reset are bounded register operations: never wait for
  // hardware. They must stop all buffer access even when returning an error.
  // Backend callbacks cannot invoke application code; IRQ wrappers call
  // interrupt() after vendor handling returns. pend() schedules that wrapper,
  // never calls it inline.
  //
  // Timing supplies now(), arm(absolute_us, callback), cancel(callback). Timer
  // callbacks run in the same serialized interrupt domain as the bus IRQ.
  // Critical supplies try_enter() plus nesting enter()/leave(). Submission
  // only uses try_enter(); MCU critical sections mask interrupts without
  // waiting. All dependencies outlive this controller and its borrowed device
  // handles.
  template <typename Backend, typename Timing, typename Critical, std::size_t N>
  class Controller {
   public:
    using Action = typename Backend::Action;
    using Config = typename Backend::Config;
    using TransferResult = Result<Action>;
    using TransferCallback = Callback<TransferResult>;
    using Counter = detail::Counters<Critical>;
    static_assert(N > 0);
    static constexpr std::size_t kDevices = N;

    auto chip_selects() const {
      std::array<PinIdentity, N> pins{};
      if constexpr (requires { backend_.chip_select(configs_[0]); }) {
        for (std::size_t i = 0; i < N; ++i) {
          pins[i] = backend_.chip_select(configs_[i]);
        }
      }
      return pins;
    }

    constexpr Controller(Backend& backend, Timing& timing, Critical& critical,
                         std::array<Config, N> devices)
        : backend_(backend),
          timing_(timing),
          critical_(critical),
          configs_(devices) {}

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    Status validate() const { return backend_.validate(configs_); }

    auto invalid_device() const { return backend_.invalid_device(); }

    const void* identity() const { return backend_.identity(); }

    // Task context, before submission. Validate the complete controller table
    // before touching hardware; failed init leaves every handle unavailable.
    Status init() {
      if (initialized_) {
        return Status::busy;
      }
      const auto validation = validate();
      if (validation != Status::ok) {
        return validation;
      }
      const auto status = backend_.init(configs_);
      if (status != Status::ok) {
        backend_.disable();
        return status;
      }
      initialized_ = true;
      return Status::ok;
    }

    // Rollback hook for application-owned multi-controller initialization.
    // Task context only, before transfers are allowed.
    void deinit() {
      backend_.disable();
      initialized_ = false;
    }

    // This static factory only takes an address; it may be used while wiring
    // file-scope objects without dereferencing an unconstructed dependency.
    template <std::size_t Index>
    static constexpr Device<Action> bind(Controller& owner) {
      static_assert(Index < N, "Unknown controller device index");
      return {&owner, Index, &kOperations};
    }

    template <std::size_t Index>
    constexpr Device<Action> device() {
      return bind<Index>(*this);
    }

    Status reset(Callback<ResetResult> callback,
                 std::optional<Duration> timeout = std::nullopt) {
      if (!callback || (timeout && !timeout->valid())) {
        return Status::invalid_argument;
      }
      if (!initialized_) {
        return Status::not_initialized;
      }
      if (!TryLock()) {
        return Status::busy;
      }
      if (active_) {
        Unlock();
        return Status::busy;
      }
      auto deadline = timing_.now();
      const auto delay =
          timeout ? timeout->microseconds() : detail::kResetTimeoutUs;
      if (!detail::Add(deadline, delay)) {
        Unlock();
        return Status::invalid_argument;
      }
      if (timing_.arm(deadline, Alarm()) != Status::ok) {
        Unlock();
        return Status::timer_error;
      }
      deadline_ = deadline;
      reset_callback_ = callback;
      resetting_ = true;
      active_ = true;
      counters_.increment(Counter::resets, critical_);
      Unlock();
      backend_.pend();
      return Status::ok;
    }

    // Called in interrupt context. Every entry reevaluates current deadlines,
    // so a previously selected timer notification is harmless after reuse:
    // it carries no old result and cannot complete a new operation early.
    void interrupt() {
      if (!TryLock()) {
        backend_.pend();
        return;
      }
      // Bound software work per IRQ even for long sequences of tiny actions.
      for (std::size_t i = 0; i < 8 && active_; ++i) {
        again_ = false;
        machine_.tick(*this);
        if (!again_) {
          break;
        }
      }
      const bool more = again_ && active_;
      const auto callback = notification_;
      const auto result = notification_result_;
      const auto reset_callback = reset_notification_;
      const auto reset_result = reset_result_;
      notification_ = {};
      reset_notification_ = {};
      Unlock();
      if (more) {
        backend_.pend();
      }
      // No access to controller transfer fields after application code starts.
      if (callback) {
        callback(result);
      }
      if (reset_callback) {
        reset_callback(reset_result);
      }
    }

    // Task-context snapshots/clears. Device and controller counters have
    // independent epochs. Individual fields are synchronized samples.
    Statistics statistics() { return counters_.snapshot(critical_); }

    void clear_statistics() { counters_.snapshot(critical_, true); }

    bool faulted() const { return faulted_.load(std::memory_order_acquire); }

   private:
    enum class State { idle, transfer, pause, finish, reset, faulted, count };

    struct Machine
        : core::StateMachine<Machine, State, State::idle,
                             static_cast<std::size_t>(State::count)> {
      void Step(State cs, State& ns, Controller& c) {
        switch (cs) {
          case State::idle:
          case State::faulted:
            if (c.resetting_) {
              ns = State::reset;
              c.again_ = true;
            } else if (c.active_) {
              ns = State::transfer;
              c.again_ = true;
            }
            break;
          case State::transfer: {
            if (c.setup_failed_) {
              c.setup_failed_ = false;
              ns = State::finish;
              c.again_ = true;
              break;
            }
            if (c.timing_.now() >= c.deadline_) {
              c.status_ = Status::timeout;
              c.RecordActionError();
              ns = State::finish;
              c.again_ = true;
              break;
            }
            if (c.started_) {
              const auto completion = c.backend_.poll();
              if (!completion) {
                break;
              }
              if (*completion != Status::ok) {
                c.status_ = *completion;
                c.RecordActionError();
                ns = State::finish;
                c.again_ = true;
                break;
              }
              c.poll_retry_ = detail::Polls(c.actions_[c.completed_]) &&
                              !detail::PollMatched(c.actions_[c.completed_]);
              if (!c.poll_retry_) {
                ++c.completed_;
              }
              c.started_ = false;
            }
            if (c.completed_ == c.actions_.size()) {
              ns = State::finish;
              c.again_ = true;
              break;
            }
            const auto& action = c.actions_[c.completed_];
            if (detail::Checks(action)) {
              if (detail::Matches(action)) {
                ++c.completed_;
              } else {
                c.status_ = Status::response_mismatch;
                ns = State::finish;
              }
              c.again_ = true;
              break;
            }
            if (const auto pause = detail::Pause(action)) {
              c.pause_due_ = c.timing_.now();
              if (!detail::Add(c.pause_due_, pause) ||
                  c.timing_.arm(c.pause_due_, c.PauseAlarm()) != Status::ok) {
                c.status_ = Status::timer_error;
                ns = State::finish;
                c.again_ = true;
              } else {
                ns = State::pause;
              }
              break;
            }
            c.RecordAttempts(action);
            c.started_ = true;
            c.backend_action_ = detail::TransferAction(action);
            // Polling is SPI-only: finish() releases CS after a match.
            // I2C's last/AUTOEND handling therefore remains unchanged.
            const auto status = c.backend_.start(
                c.backend_action_, c.completed_ == 0 && !c.poll_retry_,
                c.completed_ + 1 == c.actions_.size() &&
                    !detail::Polls(action));
            if (status != Status::ok) {
              c.status_ = status;
              c.RecordActionError();
              ns = State::finish;
              c.again_ = true;
            }
            break;
          }
          case State::pause:
            if (c.timing_.now() >= c.deadline_) {
              c.status_ = Status::timeout;
              ns = State::finish;
              c.again_ = true;
            } else if (c.timing_.now() >= c.pause_due_) {
              ++c.completed_;
              ns = State::transfer;
              c.again_ = true;
            }
            break;
          case State::finish: {
            const auto cleanup = c.backend_.finish();
            c.timing_.cancel(c.Alarm());
            c.timing_.cancel(c.PauseAlarm());
            if (cleanup != Status::ok) {
              c.counters_.increment(Counter::cleanup_failures, c.critical_);
              if (c.status_ == Status::ok) {
                c.status_ = Status::hardware_error;
              }
            }
            c.faulted_.store(cleanup != Status::ok, std::memory_order_release);
            c.Record(Counter::completed);
            if (c.status_ != Status::ok) {
              c.Record(Counter::failed);
            }
            if (c.status_ == Status::timeout) {
              c.Record(Counter::timed_out);
            }
            c.notification_ = c.callback_;
            c.notification_result_ = {c.status_, c.completed_, c.actions_};
            c.actions_ = {};
            c.callback_ = {};
            c.active_ = false;
            ns = cleanup == Status::ok ? State::idle : State::faulted;
            break;
          }
          case State::reset: {
            const auto reset = c.backend_.reset();
            auto result = reset;
            if (c.timing_.now() >= c.deadline_) {
              result = Status::timeout;
            }
            c.timing_.cancel(c.Alarm());
            c.faulted_.store(result != Status::ok, std::memory_order_release);
            if (result != Status::ok) {
              c.counters_.increment(Counter::reset_failures, c.critical_);
            }
            c.reset_notification_ = c.reset_callback_;
            c.reset_result_ = {result};
            c.reset_callback_ = {};
            c.resetting_ = false;
            c.active_ = false;
            ns = result == Status::ok ? State::idle : State::faulted;
            break;
          }
          case State::count:
            break;
        }
      }
    };

    void AlarmFired() { interrupt(); }

    void PauseFired() { interrupt(); }

    core::TimerCallback Alarm() {
      return core::TimerCallback::bind<&Controller::AlarmFired>(*this);
    }

    core::TimerCallback PauseAlarm() {
      return core::TimerCallback::bind<&Controller::PauseFired>(*this);
    }

    bool TryLock() {
      if (!critical_.try_enter()) {
        return false;
      }
      if (locked_.test_and_set(std::memory_order_acquire)) {
        critical_.leave();
        return false;
      }
      return true;
    }

    void Unlock() {
      locked_.clear(std::memory_order_release);
      critical_.leave();
    }

    void Record(typename Counter::Index counter) {
      counters_.increment(counter, critical_);
      device_counters_[device_].increment(counter, critical_);
    }

    void RecordAttempts(const Action& action) {
      if (detail::Reads(action)) {
        Record(Counter::read_attempts);
      }
      if (detail::Writes(action)) {
        Record(Counter::write_attempts);
      }
    }

    void RecordActionError() {
      if (!started_ || completed_ >= actions_.size()) {
        return;
      }
      if (detail::Reads(actions_[completed_])) {
        Record(Counter::read_errors);
      }
      if (detail::Writes(actions_[completed_])) {
        Record(Counter::write_errors);
      }
    }

    Status Reject(std::size_t index, Status status) {
      counters_.increment(Counter::rejected, critical_);
      device_counters_[index].increment(Counter::rejected, critical_);
      return status;
    }

    Status Start(std::size_t index, std::span<const Action> actions,
                 TransferCallback callback, std::optional<Duration> timeout) {
      if (!initialized_) {
        return Reject(index, Status::not_initialized);
      }
      if (!TryLock()) {
        return Reject(index, Status::busy);
      }
      if (active_) {
        Unlock();
        return Reject(index, Status::busy);
      }
      if (faulted()) {
        Unlock();
        return Reject(index, Status::faulted);
      }
      std::uint64_t delay = 0;
      auto status = callback ? detail::Timeout(actions, backend_.rate(index),
                                               timeout, delay)
                             : Status::invalid_argument;
      if (status == Status::ok) {
        for (const auto& action : actions) {
          status =
              detail::Checks(action)
                  ? Status::ok
                  : backend_.validate_action(detail::TransferAction(action));
          if (status != Status::ok) {
            break;
          }
        }
      }
      auto deadline = timing_.now();
      if (status == Status::ok && !detail::Add(deadline, delay)) {
        status = Status::invalid_argument;
      }
      if (status == Status::ok &&
          timing_.arm(deadline, Alarm()) != Status::ok) {
        status = Status::timer_error;
      }
      if (status != Status::ok) {
        Unlock();
        return Reject(index, status);
      }
      deadline_ = deadline;
      actions_ = actions;
      poll_retry_ = false;
      callback_ = callback;
      device_ = index;
      completed_ = 0;
      started_ = false;
      resetting_ = false;
      status_ = Status::ok;
      setup_failed_ = false;
      active_ = true;
      Record(Counter::accepted);
      // Configure/assert CS only after validation and a protected acceptance.
      // Backend begin is bounded and must not call application code.
      status_ = backend_.begin(index, actions);
      if (status_ != Status::ok) {
        // Preserve asynchronous completion semantics for setup failures.
        setup_failed_ = true;
      }
      Unlock();
      backend_.pend();
      return Status::ok;
    }

    inline static const typename Device<Action>::Operations kOperations{
        [](void* self, std::size_t index, std::span<const Action> actions,
           TransferCallback callback, std::optional<Duration> timeout) {
          return static_cast<Controller*>(self)->Start(index, actions, callback,
                                                       timeout);
        },
        [](void* self, std::size_t index) {
          auto& c = *static_cast<Controller*>(self);
          return c.device_counters_[index].snapshot(c.critical_);
        },
        [](void* self, std::size_t index) {
          auto& c = *static_cast<Controller*>(self);
          c.device_counters_[index].snapshot(c.critical_, true);
        }};

    Backend& backend_;
    Timing& timing_;
    Critical& critical_;
    std::array<Config, N> configs_;
    std::array<Counter, N> device_counters_{};
    Counter counters_;
    std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> initialized_{false}, faulted_{false};
    Machine machine_;
    bool active_ = false, resetting_ = false, started_ = false, again_ = false;
    bool setup_failed_ = false;
    std::size_t device_ = 0, completed_ = 0;
    std::uint64_t deadline_ = 0, pause_due_ = 0;
    Action backend_action_{};
    bool poll_retry_ = false;
    std::span<const Action> actions_;
    TransferCallback callback_, notification_;
    TransferResult notification_result_{};
    Callback<ResetResult> reset_callback_, reset_notification_;
    ResetResult reset_result_{};
    Status status_ = Status::ok;
  };


}  // namespace daveos::hal
