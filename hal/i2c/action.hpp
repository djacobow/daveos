#pragma once

#include "hal/transaction.hpp"

namespace daveos::hal::i2c {


  enum class Operation : std::uint8_t { write, read, probe };

  struct Action {
    Operation operation;
    std::span<const std::uint8_t> tx{};
    std::span<std::uint8_t> rx{};
  };

  // Unshifted seven-bit address, without an R/W bit. Only 0x08..0x77 are
  // accepted; the backend performs vendor-specific shifting/encoding.
  struct Address {
    std::uint8_t value;

    constexpr bool valid() const { return value >= 0x08 && value <= 0x77; }
  };

  constexpr Action write(std::span<const std::uint8_t> bytes) {
    return {Operation::write, bytes};
  }

  constexpr Action read(std::span<std::uint8_t> bytes) {
    return {Operation::read, {}, bytes};
  }

  // Standalone address-only write followed by STOP. Reports nack when no
  // device acknowledges; no data byte is sent or received. Reserved addresses
  // remain excluded by Address::valid().
  constexpr Action probe() { return {Operation::probe}; }

  using Result = hal::Result<Action>;
  using Callback = hal::Callback<Result>;
  using Device = hal::Device<Action>;
  using Completion = hal::Completion<Action>;


}  // namespace daveos::hal::i2c
