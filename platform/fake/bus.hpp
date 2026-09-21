#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "core/platform/timer_callback.hpp"
#include "hal/detail/actions.hpp"

namespace daveos::platform::fake {


  // A separate lock per controller. Blocking enter is for task-side counter
  // fallback only; submission and IRQ service use try_enter and never wait.
  class BusCritical {
   public:
    bool try_enter() { return mutex_.try_lock(); }

    void enter() { mutex_.lock(); }

    void leave() { mutex_.unlock(); }

   private:
    std::recursive_mutex mutex_;
  };

  // Deterministic injected alarms. The test owns time and invokes callbacks
  // returned by take_due() inside its simulated interrupt domain. Splitting
  // selection and invocation deliberately permits stale-callback race tests.
  // A shared clock serializes alarm storage; arm uses try-lock so submission
  // can fail rather than block behind another controller. No callbacks run
  // under this lock. cancel/take_due are bounded host critical sections.
  template <std::size_t Capacity = 16>
  class BusClock {
   public:
    std::uint64_t now() const { return now_.load(); }

    void advance(std::uint64_t microseconds) { now_.fetch_add(microseconds); }

    hal::Status arm(std::uint64_t due, const core::TimerCallback& callback) {
      std::unique_lock lock(alarms_mutex_, std::try_to_lock);
      if (!lock.owns_lock() || fail_arm || due <= now_ || !callback) {
        return hal::Status::timer_error;
      }
      Alarm* free = nullptr;
      for (auto& alarm : alarms_) {
        if (alarm.callback == callback) {
          alarm = {due, callback};
          return hal::Status::ok;
        }
        if (!alarm.callback) {
          free = &alarm;
        }
      }
      if (!free) {
        return hal::Status::timer_error;
      }
      *free = {due, callback};
      return hal::Status::ok;
    }

    void cancel(const core::TimerCallback& callback) {
      std::lock_guard lock(alarms_mutex_);
      for (auto& alarm : alarms_) {
        if (alarm.callback == callback) {
          alarm.callback = {};
        }
      }
    }

    core::TimerCallback take_due() {
      std::lock_guard lock(alarms_mutex_);
      Alarm* first = nullptr;
      for (auto& alarm : alarms_) {
        if (alarm.callback && alarm.due <= now_ &&
            (!first || alarm.due < first->due)) {
          first = &alarm;
        }
      }
      if (!first) {
        return {};
      }
      const auto callback = first->callback;
      first->callback = {};
      return callback;
    }

    std::size_t pending() const {
      std::lock_guard lock(alarms_mutex_);
      return std::count_if(alarms_.begin(), alarms_.end(),
                           [](const auto& a) { return bool(a.callback); });
    }

    std::atomic<bool> fail_arm{false};

   private:
    struct Alarm {
      std::uint64_t due = 0;
      core::TimerCallback callback;
    };

    std::atomic<std::uint64_t> now_{0};
    mutable std::mutex alarms_mutex_;
    std::array<Alarm, Capacity> alarms_{};
  };

  struct SpiDeviceConfig {
    std::uint32_t chip_select;
    std::uint32_t rate = 100000;
  };

  struct I2cDeviceConfig {
    hal::i2c::Address address;
    std::uint32_t rate = 100000;
  };

  template <typename A, typename C, std::size_t Devices = 8>
  class ScriptedBus {
   public:
    using Action = A;
    using Config = C;

    struct Step {
      Action expected;
      // RX bytes supplied by the script, independent of caller RX storage.
      std::span<const std::uint8_t> receive{};
      hal::Status result = hal::Status::ok;
      bool hang = false;
    };

    struct Trace {
      std::size_t device;
      decltype(Action::operation) operation;
      bool first, last, selected;
      std::uint8_t address = 0;
    };

    const void* identity() const { return this; }

    static hal::PinIdentity chip_select(const Config& config) {
      if constexpr (std::same_as<Action, hal::spi::Action>) {
        return {1, config.chip_select};
      } else {
        return {};
      }
    }

    void script(std::span<const Step> steps) {
      steps_ = steps;
      position_ = 0;
    }

    hal::Status validate(std::span<const Config> configs) const {
      invalid_device_.reset();
      if (configs.empty() || configs.size() > Devices) {
        return hal::Status::invalid_argument;
      }
      for (std::size_t i = 0; i < configs.size(); ++i) {
        invalid_device_ = i;
        if (!configs[i].rate) {
          return hal::Status::invalid_argument;
        }
        if constexpr (std::same_as<Action, hal::i2c::Action>) {
          if (!configs[i].address.valid()) {
            return hal::Status::invalid_argument;
          }
          if (configs[i].rate != configs[0].rate) {
            return hal::Status::invalid_argument;
          }
        }
        for (std::size_t j = 0; j < i; ++j) {
          if constexpr (std::same_as<Action, hal::spi::Action>) {
            if (configs[i].chip_select == configs[j].chip_select) {
              return hal::Status::invalid_argument;
            }
          } else {
            if (configs[i].address.value == configs[j].address.value) {
              return hal::Status::invalid_argument;
            }
          }
        }
      }
      invalid_device_.reset();
      return hal::Status::ok;
    }

    std::optional<std::size_t> invalid_device() const {
      return invalid_device_;
    }

    hal::Status init(std::span<const Config> configs) {
      std::copy(configs.begin(), configs.end(), configs_.begin());
      return initialization_result;
    }

    void disable() {
      selected = false;
      active_.reset();
      disabled = true;
    }

    std::uint32_t rate(std::size_t index) const { return configs_[index].rate; }

    hal::Status validate_action(const Action&) const { return buffer_result; }

    hal::Status begin(std::size_t index, std::span<const Action> actions) {
      device_ = index;
      if constexpr (std::same_as<Action, hal::i2c::Action>) {
        address_ = configs_[index].address.value;
      }
      ++begins;
      selected = true;
      if constexpr (std::same_as<Action, hal::spi::Action>) {
        selected =
            actions.front().operation != hal::spi::Operation::idle_clocks;
      }
      return begin_result;
    }

    hal::Status begin_probe(hal::i2c::Address address)
      requires std::same_as<Action, hal::i2c::Action>
    {
      address_ = address.value;
      device_ = Devices;
      ++begins;
      selected = true;
      return begin_result;
    }

    hal::Status start(const Action& action, bool first, bool last) {
      if (trace_count == trace.size()) {
        return hal::Status::hardware_error;
      }
      trace[trace_count++] = {device_, action.operation, first,
                              last,    selected,         address_};
      if (position_ == steps_.size()) {
        return hal::Status::hardware_error;
      }
      const auto& step = steps_[position_++];
      if (action.operation != step.expected.operation ||
          !std::equal(action.tx.begin(), action.tx.end(),
                      step.expected.tx.begin(), step.expected.tx.end()) ||
          action.rx.size() != step.receive.size()) {
        return hal::Status::hardware_error;
      }
      if constexpr (std::same_as<Action, hal::spi::Action>) {
        if (action.fill != step.expected.fill ||
            action.amount != step.expected.amount) {
          return hal::Status::hardware_error;
        }
      }
      active_ = Active{action, step};
      if (!step.hang) {
        pend();
      }
      return hal::Status::ok;
    }

    std::optional<hal::Status> poll() {
      if (!active_ || active_->step.hang) {
        return std::nullopt;
      }
      std::copy(active_->step.receive.begin(), active_->step.receive.end(),
                active_->action.rx.begin());
      const auto status = active_->step.result;
      active_.reset();
      return status;
    }

    hal::Status finish() {
      active_.reset();
      selected = false;
      ++finishes;
      return cleanup_result;
    }

    hal::Status reset() {
      active_.reset();
      selected = false;
      return reset_result;
    }

    void pend() { pending_.store(true, std::memory_order_release); }

    bool take_pending() {
      return pending_.exchange(false, std::memory_order_acq_rel);
    }

    std::array<Trace, 128> trace{};
    std::size_t trace_count = 0, begins = 0, finishes = 0;
    bool selected = false, disabled = false;
    hal::Status initialization_result = hal::Status::ok,
                buffer_result = hal::Status::ok;
    hal::Status begin_result = hal::Status::ok,
                cleanup_result = hal::Status::ok;
    hal::Status reset_result = hal::Status::ok;

   private:
    struct Active {
      Action action;
      Step step;
    };

    mutable std::optional<std::size_t> invalid_device_;
    std::array<Config, Devices> configs_{};
    std::span<const Step> steps_{};
    std::size_t position_ = 0, device_ = 0;
    std::uint8_t address_ = 0;
    std::optional<Active> active_;
    std::atomic<bool> pending_{false};
  };

  using SpiBus = ScriptedBus<hal::spi::Action, SpiDeviceConfig>;
  using I2cBus = ScriptedBus<hal::i2c::Action, I2cDeviceConfig>;


}  // namespace daveos::platform::fake
