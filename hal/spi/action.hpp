#pragma once

#include "hal/transaction.hpp"

namespace daveos::hal::spi {


  enum class Operation : std::uint8_t {
    write,
    read,
    exchange,
    pause,
    idle_clocks,
    check_response
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

  // Examine a previously received response without clocking or releasing CS.
  // Skip leading idle bytes; compare the first response under mask. At most
  // 32 bytes are scanned in interrupt context. No match aborts the transaction.
  constexpr Action check_response(std::span<const std::uint8_t> bytes,
                                  std::uint8_t expected,
                                  std::uint8_t mask = 0xff,
                                  std::uint8_t idle = 0xff) {
    return {Operation::check_response,
            bytes,
            {},
            std::uint64_t{expected} | (std::uint64_t{mask} << 8),
            idle};
  }

  using Result = hal::Result<Action>;
  using Callback = hal::Callback<Result>;
  using Device = hal::Device<Action>;
  using Completion = hal::Completion<Action>;


}  // namespace daveos::hal::spi
