#pragma once

#include <array>

#include "hal/spi/action.hpp"

namespace daveos::storage::sd {


  // wire contains R1, data token, payload, then two CRC bytes. Caller supplies
  // exactly payload_size+4 bytes. Each response is consumed one byte at a time
  // so payload bytes are never lost, even with an immediately available token.
  // Keep CS asserted through all actions and supply a transaction timeout.
  // Invalid wire spans are rejected by HAL validation, without touching a bus.
  inline auto read_actions(std::span<const std::uint8_t> command,
                           std::span<std::uint8_t> wire) {
    namespace spi = hal::spi;
    if (wire.size() < 5) {
      wire = {};
    }
    const auto response = wire.empty() ? wire : wire.first(1);
    const auto token = wire.empty() ? wire : wire.subspan(1, 1);
    return std::array{spi::write(command),
                      spi::read_until(response, 0xff, 8),
                      spi::check_response(response, 0),
                      spi::read_until(token),
                      spi::check_response(token, 0xfe),
                      spi::read(wire.empty() ? wire : wire.subspan(2))};
  }


}  // namespace daveos::storage::sd
