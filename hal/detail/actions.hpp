#pragma once

#include <algorithm>
#include <limits>

#include "hal/i2c/action.hpp"
#include "hal/spi/action.hpp"

namespace daveos::hal::detail {


  inline constexpr std::uint64_t kTimeoutMinimumUs = 10000;
  inline constexpr std::uint64_t kTimeoutMarginUs = 1000;
  inline constexpr std::uint64_t kTimeoutFactor = 4;
  inline constexpr std::uint64_t kResetTimeoutUs = 100000;
  inline constexpr auto kMaximumTime = core::kForever - 1;

  constexpr bool Add(std::uint64_t& value, std::uint64_t amount) {
    if (value > kMaximumTime || amount > kMaximumTime - value) {
      return false;
    }
    value += amount;
    return true;
  }

  constexpr bool Multiply(std::uint64_t& value, std::uint64_t amount) {
    if (amount && value > kMaximumTime / amount) {
      return false;
    }
    value *= amount;
    return true;
  }

  inline bool Overlap(std::span<const std::uint8_t> tx,
                      std::span<std::uint8_t> rx) {
    const auto a = reinterpret_cast<std::uintptr_t>(tx.data());
    const auto b = reinterpret_cast<std::uintptr_t>(rx.data());
    // Subtraction avoids wrapping an end address.
    return a <= b ? b - a < tx.size() : a - b < rx.size();
  }

  inline bool Reads(const spi::Action& a) {
    return a.operation == spi::Operation::poll_response ||
           a.operation == spi::Operation::read ||
           a.operation == spi::Operation::exchange;
  }

  inline bool Writes(const spi::Action& a) {
    return a.operation == spi::Operation::write ||
           a.operation == spi::Operation::exchange;
  }

  inline bool Reads(const i2c::Action& a) {
    return a.operation == i2c::Operation::read;
  }

  inline bool Writes(const i2c::Action& a) {
    return a.operation == i2c::Operation::write;
  }

  inline bool Polls(const spi::Action& a) {
    return a.operation == spi::Operation::poll_response;
  }

  inline bool Polls(const i2c::Action&) { return false; }

  inline bool PollMatched(const spi::Action& a) {
    return (a.rx.back() & static_cast<std::uint8_t>(a.amount >> 8)) ==
           static_cast<std::uint8_t>(a.amount);
  }

  inline bool PollMatched(const i2c::Action&) { return false; }

  inline spi::Action TransferAction(const spi::Action& a) {
    return Polls(a) ? spi::read(a.rx, a.fill) : a;
  }

  inline i2c::Action TransferAction(const i2c::Action& a) { return a; }

  inline bool Checks(const spi::Action& a) {
    return a.operation == spi::Operation::check_response;
  }

  inline bool Checks(const i2c::Action&) { return false; }

  inline bool Matches(const spi::Action& a) {
    const auto mask = static_cast<std::uint8_t>(a.amount >> 8);
    for (auto byte : a.tx) {
      if (byte != a.fill) {
        return (byte & mask) == static_cast<std::uint8_t>(a.amount);
      }
    }
    return false;
  }

  inline bool Matches(const i2c::Action&) { return false; }

  inline std::uint64_t Pause(const spi::Action& a) {
    return a.operation == spi::Operation::pause ? a.amount : 0;
  }

  inline std::uint64_t Pause(const i2c::Action&) { return 0; }

  inline Status Measure(const spi::Action& a, std::size_t count,
                        std::uint64_t& bits, std::uint64_t& pauses) {
    if (!a.valid) {
      return Status::invalid_argument;
    }
    std::uint64_t n = 0;
    switch (a.operation) {
      case spi::Operation::write:
        if (a.tx.empty() || !a.rx.empty()) {
          return Status::invalid_argument;
        }
        n = a.tx.size();
        break;
      case spi::Operation::read:
        if (a.rx.empty() || !a.tx.empty()) {
          return Status::invalid_argument;
        }
        n = a.rx.size();
        break;
      case spi::Operation::exchange:
        if (a.tx.empty() || a.tx.size() != a.rx.size() || Overlap(a.tx, a.rx)) {
          return Status::invalid_argument;
        }
        n = a.tx.size();
        break;
      case spi::Operation::poll_response:
        if (a.rx.empty() || a.rx.size() > 32 || !a.tx.empty() ||
            a.amount > 0xffff || !(a.amount >> 8) ||
            (static_cast<std::uint8_t>(a.amount) & ~(a.amount >> 8))) {
          return Status::invalid_argument;
        }
        n = a.rx.size();
        break;
      case spi::Operation::check_response:
        return !a.tx.empty() && a.tx.size() <= 32 && a.rx.empty() &&
                       a.amount <= 0xffff && (a.amount >> 8) &&
                       !(static_cast<std::uint8_t>(a.amount) & ~(a.amount >> 8))
                   ? Status::ok
                   : Status::invalid_argument;
      case spi::Operation::pause:
        return a.amount && a.tx.empty() && a.rx.empty() && Add(pauses, a.amount)
                   ? Status::ok
                   : Status::invalid_argument;
      case spi::Operation::idle_clocks:
        return count == 1 && a.amount && a.amount % 8 == 0 && a.tx.empty() &&
                       a.rx.empty() && a.fill == 0xff && Add(bits, a.amount)
                   ? Status::ok
                   : Status::invalid_argument;
      default:
        return Status::invalid_argument;
    }
    return Multiply(n, 8) && Add(bits, n) ? Status::ok
                                          : Status::invalid_argument;
  }

  inline Status Measure(const i2c::Action& a, std::size_t count,
                        std::uint64_t& bits, std::uint64_t&) {
    if (a.operation == i2c::Operation::probe) {
      return count == 1 && a.tx.empty() && a.rx.empty() && Add(bits, 11)
                 ? Status::ok
                 : Status::invalid_argument;
    }
    if ((!Reads(a) && !Writes(a)) ||
        (Reads(a) && (a.rx.empty() || !a.tx.empty())) ||
        (Writes(a) && (a.tx.empty() || !a.rx.empty()))) {
      return Status::invalid_argument;
    }
    std::uint64_t n = Reads(a) ? a.rx.size() : a.tx.size();
    // One address byte with ACK, nine bits per data byte, plus conservative
    // START/STOP timing allowance per phase. Stretching is covered by margin
    // or by an explicit caller timeout, never assumed unbounded.
    return Add(n, 1) && Multiply(n, 9) && Add(n, 2) && Add(bits, n)
               ? Status::ok
               : Status::invalid_argument;
  }

  template <typename Action>
  Status Timeout(std::span<const Action> actions, std::uint32_t rate,
                 std::optional<Duration> requested, std::uint64_t& result) {
    if (actions.empty() || !rate || (requested && !requested->valid())) {
      return Status::invalid_argument;
    }
    std::uint64_t bits = 0, pauses = 0;
    for (const auto& action : actions) {
      if (Polls(action) && !requested) {
        return Status::invalid_argument;
      }
      if (Measure(action, actions.size(), bits, pauses) != Status::ok) {
        return Status::invalid_argument;
      }
    }
    if (requested) {
      result = requested->microseconds();
      return Status::ok;
    }
    // Divide before multiplying so large but representable transfers work.
    std::uint64_t wire = bits / rate;
    const auto remainder = bits % rate;
    if (!Multiply(wire, 1000000) ||
        !Add(wire, (remainder * 1000000 + rate - 1) / rate) ||
        !Multiply(wire, kTimeoutFactor) || !Add(wire, kTimeoutMarginUs)) {
      return Status::invalid_argument;
    }
    result = std::max(kTimeoutMinimumUs, wire);
    return Add(result, pauses) ? Status::ok : Status::invalid_argument;
  }


}  // namespace daveos::hal::detail
