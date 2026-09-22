#pragma once

#include "hal/transaction.hpp"

namespace daveos::hal::spi {


  enum class Operation : std::uint8_t {
    write,
    read,
    exchange,
    pause,
    idle_clocks,
    check_response,
    poll_response,
    read_until
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

  // Repeatedly read a 1..32-byte window, with CS held, until its LAST byte
  // matches under mask. Earlier bytes may straddle a busy-release transition.
  // Requires an explicit whole-transaction timeout. Window is borrowed and
  // overwritten on each poll; no application callback runs between windows.
  constexpr Action poll_response(std::span<std::uint8_t> window,
                                 std::uint8_t expected,
                                 std::uint8_t mask = 0xff,
                                 std::uint8_t fill = 0xff) {
    return {Operation::poll_response,
            {},
            window,
            std::uint64_t{expected} | (std::uint64_t{mask} << 8),
            fill};
  }

  // Read one byte at a time, stopping at the first non-idle byte without
  // consuming any following payload. Requires a whole-transaction timeout.
  // A nonzero byte limit rejects an idle-only response as response_mismatch.
  constexpr Action read_until(std::span<std::uint8_t> response,
                              std::uint8_t idle = 0xff,
                              std::uint32_t maximum_bytes = 0) {
    return {Operation::read_until, {}, response, maximum_bytes, idle};
  }

  using Result = hal::Result<Action>;
  using Callback = hal::Callback<Result>;
  using Device = hal::Device<Action>;
  using Completion = hal::Completion<Action>;


}  // namespace daveos::hal::spi
