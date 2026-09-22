#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace daveos::storage::sd {


  constexpr std::uint32_t little(std::span<const std::uint8_t> b) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < b.size() && i < 4; ++i) {
      value |= std::uint32_t{b[i]} << (8 * i);
    }
    return value;
  }

  constexpr std::uint16_t crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0;
    for (auto byte : bytes) {
      crc ^= static_cast<std::uint16_t>(byte << 8);
      for (std::uint8_t bit = 0; bit < 8; ++bit) {
        crc = static_cast<std::uint16_t>((crc << 1) ^
                                         ((crc & 0x8000) ? 0x1021 : 0));
      }
    }
    return crc;
  }

  // The command response must be within eight bytes. A data token must arrive
  // within the caller's clock budget; reject error tokens, truncation and CRC
  // failures rather than scanning past them for something resembling data.
  inline std::optional<std::span<const std::uint8_t>> data(
      std::span<const std::uint8_t> wire, std::size_t length,
      std::size_t wait_bytes) {
    std::size_t pos = 0;
    while (pos < wire.size() && pos < 8 && wire[pos] == 0xff) {
      ++pos;
    }
    if (pos >= wire.size() || pos == 8 || wire[pos++] != 0) {
      return std::nullopt;
    }
    const auto end = pos + wait_bytes;
    while (pos < wire.size() && pos < end && wire[pos] == 0xff) {
      ++pos;
    }
    if (pos >= wire.size() || pos >= end || wire[pos++] != 0xfe ||
        length > wire.size() - pos || wire.size() - pos - length < 2) {
      return std::nullopt;
    }
    auto payload = wire.subspan(pos, length);
    const auto expected = static_cast<std::uint16_t>((wire[pos + length] << 8) |
                                                     wire[pos + length + 1]);
    return crc16(payload) == expected ? std::optional{payload} : std::nullopt;
  }

  struct Card {
    std::uint64_t sectors;
    std::uint32_t maximum_hz;
  };

  // This fixture deliberately supports CSD v2 (SDHC/SDXC) only.
  inline std::optional<Card> card(std::span<const std::uint8_t> csd) {
    if (csd.size() != 16 || (csd[0] >> 6) != 1 || (csd[5] & 15) != 9) {
      return std::nullopt;
    }
    constexpr std::array<std::uint32_t, 4> units{100000, 1000000, 10000000,
                                                 100000000};
    constexpr std::array<std::uint8_t, 16> factors{
        0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80};
    const auto speed = csd[3];
    if ((speed & 0x80) || (speed & 7) >= units.size() ||
        !factors[(speed >> 3) & 15]) {
      return std::nullopt;
    }
    const auto size = (std::uint32_t{csd[7] & 0x3fu} << 16) |
                      (std::uint32_t{csd[8]} << 8) | csd[9];
    return Card{(std::uint64_t{size} + 1) * 1024,
                units[speed & 7] / 10 * factors[(speed >> 3) & 15]};
  }

  // All command packets have a valid CRC7, even when card CRC checking is off.
  constexpr std::array<std::uint8_t, 6> command(std::uint8_t index,
                                                std::uint32_t argument) {
    std::array<std::uint8_t, 6> bytes{static_cast<std::uint8_t>(0x40 | index),
                                      static_cast<std::uint8_t>(argument >> 24),
                                      static_cast<std::uint8_t>(argument >> 16),
                                      static_cast<std::uint8_t>(argument >> 8),
                                      static_cast<std::uint8_t>(argument),
                                      0};
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < 5; ++i) {
      auto value = bytes[i];
      for (std::uint8_t bit = 0; bit < 8; ++bit) {
        crc = static_cast<std::uint8_t>(crc << 1);
        if ((value ^ crc) & 0x80) {
          crc ^= 0x09;
        }
        value = static_cast<std::uint8_t>(value << 1);
      }
    }
    bytes[5] = static_cast<std::uint8_t>((crc << 1) | 1);
    return bytes;
  }


}  // namespace daveos::storage::sd
