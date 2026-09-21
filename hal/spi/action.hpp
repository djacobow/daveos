#pragma once

#include "hal/transaction.hpp"

namespace daveos::hal::spi {


  enum class Operation : std::uint8_t {
    write,
    read,
    exchange,
    pause,
    idle_clocks
  };

  // Borrowed buffers: exchange spans must be equal-sized and nonoverlapping.
  // Pauses hold CS; idle clocks are a standalone transaction with CS inactive.
  struct Action {
    Operation operation;
    std::span<const std::uint8_t> tx{};
    std::span<std::uint8_t> rx{};
    std::uint64_t amount = 0;
    std::uint8_t fill = 0xff;
    bool valid = true;
  };

  constexpr Action write(std::span<const std::uint8_t> bytes) {
    return {Operation::write, bytes};
  }

  constexpr Action read(std::span<std::uint8_t> bytes,
                        std::uint8_t fill = 0xff) {
    return {Operation::read, {}, bytes, 0, fill};
  }

  constexpr Action exchange(std::span<const std::uint8_t> tx,
                            std::span<std::uint8_t> rx) {
    return {Operation::exchange, tx, rx};
  }

  constexpr Action pause(Duration delay) {
    return {Operation::pause,     {},   {},
            delay.microseconds(), 0xff, delay.valid()};
  }

  constexpr Action idle_clocks(std::uint64_t cycles) {
    return {Operation::idle_clocks, {}, {}, cycles};
  }

  using Result = hal::Result<Action>;
  using Callback = hal::Callback<Result>;
  using Device = hal::Device<Action>;
  using Completion = hal::Completion<Action>;


}  // namespace daveos::hal::spi
