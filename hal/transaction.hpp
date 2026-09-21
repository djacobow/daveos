#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "core/schedule/duration.hpp"
#include "hal/status.h"

namespace daveos::hal {


  // Positive, exact microseconds. Invalid conversions remain invalid instead
  // of truncating. No integer constructor: units must be explicit at call
  // sites.
  class Duration {
   public:
    template <core::DurationRep Rep, typename Period>
    constexpr Duration(std::chrono::duration<Rep, Period> value) {
      valid_ =
          core::to_microseconds(value, us_) == core::Status::ok && us_ != 0;
    }

    constexpr bool valid() const { return valid_; }

    constexpr std::uint64_t microseconds() const { return us_; }

   private:
    std::uint64_t us_ = 0;
    bool valid_ = false;
  };

  // Non-owning callback; the target must survive completion. Null callbacks
  // are rejected at submission, not silently accepted and discarded.
  template <typename Result>
  struct Callback {
    void* context = nullptr;
    void (*invoke)(void*, const Result&) = nullptr;

    template <auto Function, typename Owner>
    static constexpr Callback bind(Owner& owner) {
      static_assert(
          std::same_as<
              std::invoke_result_t<decltype(Function), Owner&, const Result&>,
              void>);
      return {&owner, [](void* object, const Result& result) {
                std::invoke(Function, *static_cast<Owner*>(object), result);
              }};
    }

    explicit constexpr operator bool() const { return invoke != nullptr; }

    void operator()(const Result& result) const { invoke(context, result); }
  };

  template <typename Action>
  struct Result {
    Status status = Status::ok;
    std::size_t completed_actions = 0;
    // Original borrowed descriptors and buffers, returned without copying.
    std::span<const Action> actions;
  };

  struct ResetResult {
    Status status = Status::ok;
  };

  // Physical GPIO identity used only to validate cross-controller CS ownership.
  struct PinIdentity {
    std::uintptr_t port = 0;
    std::uint32_t pin = 0;
    friend constexpr bool operator==(PinIdentity, PinIdentity) = default;

    constexpr explicit operator bool() const { return port != 0; }
  };

  struct Statistics {
    std::uint64_t read_attempts = 0, write_attempts = 0;
    std::uint64_t read_errors = 0, write_errors = 0;
    std::uint64_t accepted = 0, completed = 0, failed = 0, timed_out = 0;
    std::uint64_t rejected = 0;
    // Controller-only diagnostics, zero in device snapshots.
    std::uint64_t cleanup_failures = 0, resets = 0, reset_failures = 0;
  };

  // One outstanding transaction; result publication is safe across task/ISR
  // contexts. Single consumer owns start()/result(); the HAL owns completion.
  // Destruction/reuse requires the transaction to have completed.
  template <typename Action>
  class Completion {
   public:
    template <typename Device>
    Status start(const Device& device, std::span<const Action> actions,
                 std::optional<Duration> timeout = std::nullopt) {
      auto previous = phase_.load(std::memory_order_acquire);
      if (previous == Phase::pending ||
          !phase_.compare_exchange_strong(previous, Phase::pending,
                                          std::memory_order_acq_rel)) {
        return Status::busy;
      }
      const auto status = device.start(
          actions,
          Callback<Result<Action>>::template bind<&Completion::Complete>(*this),
          timeout);
      if (status != Status::ok) {
        Complete({status, 0, actions});
      }
      return status;
    }

    bool ready() const {
      return phase_.load(std::memory_order_acquire) == Phase::ready;
    }

    std::optional<Result<Action>> result() const {
      if (!ready()) {
        return std::nullopt;
      }
      return result_;
    }

   private:
    // Publication flag, not a tick-driven lifecycle/state machine.
    enum class Phase : std::uint8_t { empty, pending, ready };

    void Complete(const Result<Action>& result) {
      result_ = result;
      phase_.store(Phase::ready, std::memory_order_release);
    }

    std::atomic<Phase> phase_{Phase::empty};
    Result<Action> result_{};
  };

  template <typename Action>
  class Device {
   public:
    struct Operations {
      Status (*start)(void*, std::size_t, std::span<const Action>,
                      Callback<Result<Action>>, std::optional<Duration>);
      Statistics (*statistics)(void*, std::size_t);
      void (*clear_statistics)(void*, std::size_t);
    };

    constexpr Device() = default;

    constexpr Device(void* owner, std::size_t index,
                     const Operations* operations)
        : owner_(owner), index_(index), operations_(operations) {}

    // Task/ISR safe and nonblocking. Accepted buffers/descriptors remain
    // borrowed until callback entry. Rejection never invokes the callback.
    Status start(std::span<const Action> actions,
                 Callback<Result<Action>> callback,
                 std::optional<Duration> timeout = std::nullopt) const {
      return operations_ ? operations_->start(owner_, index_, actions, callback,
                                              timeout)
                         : Status::not_initialized;
    }

    // Task context only; independent from the controller's snapshot/clear.
    Statistics statistics() const {
      return operations_ ? operations_->statistics(owner_, index_)
                         : Statistics{};
    }

    void clear_statistics() const {
      if (operations_) {
        operations_->clear_statistics(owner_, index_);
      }
    }

   private:
    void* owner_ = nullptr;
    std::size_t index_ = 0;
    const Operations* operations_ = nullptr;
  };


}  // namespace daveos::hal
