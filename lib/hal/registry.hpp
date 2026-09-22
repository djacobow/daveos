#pragma once

#include <array>
#include <tuple>
#include <type_traits>

#include "hal/transaction.hpp"

namespace daveos::hal {


  namespace detail {
    template <typename T, std::size_t N>
    constexpr bool UniqueAliases(const std::array<T, N>& aliases) {
      for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = 0; j < i; ++j) {
          if (aliases[i] == aliases[j]) {
            return false;
          }
        }
      }
      return true;
    }
  }  // namespace detail

  // Compile-time alias lookup over application-owned controller device handles.
  // Aliases identify attached devices, not independently lockable bus hardware.
  template <typename Action, auto... Aliases>
  class Registry {
    static constexpr auto kAliases = std::array{Aliases...};
    static_assert(sizeof...(Aliases) > 0);
    static_assert(std::is_enum_v<typename decltype(kAliases)::value_type>);
    static_assert(detail::UniqueAliases(kAliases),
                  "HAL device aliases must be unique");

   public:
    explicit constexpr Registry(
        std::array<Device<Action>, sizeof...(Aliases)> devices)
        : devices_(devices) {}

    template <auto Alias>
    constexpr Device<Action> device() const {
      constexpr auto index = [] {
        for (std::size_t i = 0; i < kAliases.size(); ++i) {
          if (kAliases[i] == Alias) {
            return i;
          }
        }
        return kAliases.size();
      }();
      static_assert(index < kAliases.size(), "Unknown HAL device alias");
      return devices_[index];
    }

   private:
    std::array<Device<Action>, sizeof...(Aliases)> devices_;
  };

  template <auto Alias, typename C>
  struct BusBinding {
    static constexpr auto alias = Alias;
    using Controller = C;
    C& controller;
  };

  template <auto Alias, typename C>
  constexpr auto bus(C& controller) {
    return BusBinding<Alias, C>{controller};
  }

  // Application-owned bus registry. init validates every table first, rolls
  // back partial hardware setup, and reports the failing entry by index.
  template <typename... Bindings>
  class BusRegistry {
    static constexpr auto kAliases = std::array{Bindings::alias...};
    static_assert(detail::UniqueAliases(kAliases),
                  "HAL bus aliases must be unique");

   public:
    struct Failure {
      std::size_t entry;
      Status status;
      std::optional<std::size_t> device = std::nullopt;
    };

    explicit constexpr BusRegistry(Bindings... bindings)
        : bindings_(bindings...) {}

    template <auto Alias>
    auto& controller() {
      constexpr auto index = [] {
        for (std::size_t i = 0; i < kAliases.size(); ++i) {
          if (kAliases[i] == Alias) {
            return i;
          }
        }
        return kAliases.size();
      }();
      static_assert(index < kAliases.size(), "Unknown HAL bus alias");
      return std::get<index>(bindings_).controller;
    }

    Status init() {
      if (initialized_) {
        return Status::busy;
      }
      failure_.reset();
      std::array<const void*, sizeof...(Bindings)> identities{};
      std::array<PinIdentity, (Bindings::Controller::kDevices + ...)> pins{};
      std::size_t used_pins = 0;
      std::size_t index = 0;
      std::apply(
          [&](auto&... binding) {
            (
                [&] {
                  const auto identity = binding.controller.identity();
                  auto status = binding.controller.validate();
                  for (std::size_t j = 0; j < index; ++j) {
                    if (identity == identities[j]) {
                      status = Status::invalid_argument;
                    }
                  }
                  for (const auto pin : binding.controller.chip_selects()) {
                    if (!pin) {
                      continue;
                    }
                    for (std::size_t j = 0; j < used_pins; ++j) {
                      if (pin == pins[j]) {
                        status = Status::invalid_argument;
                      }
                    }
                    pins[used_pins++] = pin;
                  }
                  identities[index] = identity;
                  if (!failure_ && status != Status::ok) {
                    failure_ = Failure{index, status,
                                       binding.controller.invalid_device()};
                  }
                  ++index;
                }(),
                ...);
          },
          bindings_);
      if (failure_) {
        return failure_->status;
      }
      index = 0;
      std::apply(
          [&](auto&... binding) {
            (
                [&] {
                  if (!failure_) {
                    const auto status = binding.controller.init();
                    if (status != Status::ok) {
                      failure_ = Failure{index, status,
                                         binding.controller.invalid_device()};
                    }
                  }
                  ++index;
                }(),
                ...);
          },
          bindings_);
      if (failure_) {
        index = 0;
        std::apply(
            [&](auto&... binding) {
              (
                  [&] {
                    if (index < failure_->entry) {
                      binding.controller.deinit();
                    }
                    ++index;
                  }(),
                  ...);
            },
            bindings_);
        return failure_->status;
      }
      initialized_ = true;
      return Status::ok;
    }

    std::optional<Failure> initialization_failure() const { return failure_; }

   private:
    std::tuple<Bindings...> bindings_;
    std::optional<Failure> failure_;
    bool initialized_ = false;
  };


}  // namespace daveos::hal
