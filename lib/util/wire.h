#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace daveos::util::wire {


  // Internal fixed-format helpers. Callers validate the enclosing record size
  // before accessing fields; no native structure padding appears on the wire.
  inline std::uint32_t read32(std::span<const std::byte> data, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      value |= std::uint32_t(std::to_integer<std::uint8_t>(data[at + i]))
               << (8 * i);
    }
    return value;
  }

  inline std::uint64_t read64(std::span<const std::byte> data, std::size_t at) {
    return read32(data, at) | (std::uint64_t(read32(data, at + 4)) << 32);
  }

  inline void write32(std::span<std::byte> data, std::size_t at,
                      std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      data[at + i] = std::byte((value >> (8 * i)) & 0xff);
    }
  }

  inline void write64(std::span<std::byte> data, std::size_t at,
                      std::uint64_t value) {
    write32(data, at, static_cast<std::uint32_t>(value));
    write32(data, at + 4, static_cast<std::uint32_t>(value >> 32));
  }


}  // namespace daveos::util::wire
