#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "util/crc32.h"

namespace daveos::otp {


  inline constexpr std::uint32_t kBlockCount = 32;
  inline constexpr std::size_t kRecordSize = 64;
  inline constexpr std::size_t kPayloadSize = 56;
  enum class Type : std::uint16_t { serial_number = 1, unwritten = 0xffff };

  // The persistent format is explicitly little endian, never a native struct.
  struct Record {
    std::array<std::byte, kRecordSize> bytes{};

    std::uint16_t type() const;
    std::uint16_t size() const;
    bool valid(const util::crc32::Service& crc = util::crc32::Service{}) const;
    std::string_view payload() const;
  };

  static_assert(sizeof(Record) == kRecordSize);
  bool valid_serial(std::string_view value);
  Record serial_record(std::string_view value, const util::crc32::Service& crc =
                                                   util::crc32::Service{});


}  // namespace daveos::otp
