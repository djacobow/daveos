#include "record.h"

#include <algorithm>

namespace daveos::otp {


  namespace {
    std::uint16_t Read16(const std::byte* p) {
      return std::uint16_t(std::to_integer<std::uint16_t>(p[0]) |
                           (std::to_integer<std::uint16_t>(p[1]) << 8));
    }

    std::uint32_t Checksum(const Record& record,
                           const util::crc32::Service& crc) {
      return crc.update(crc.update(0, std::span(record.bytes).first(4)),
                        std::span(record.bytes).subspan(8));
    }
  }  // namespace

  std::uint16_t Record::type() const { return Read16(bytes.data()); }

  std::uint16_t Record::size() const { return Read16(bytes.data() + 2); }

  bool Record::valid(const util::crc32::Service& crc) const {
    if (type() == std::uint16_t(Type::unwritten) || size() > kPayloadSize) {
      return false;
    }
    std::uint32_t stored = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      stored |= std::to_integer<std::uint32_t>(bytes[4 + i]) << (i * 8);
    }
    return stored == Checksum(*this, crc) &&
           std::all_of(bytes.begin() + 8 + size(), bytes.end(),
                       [](std::byte b) { return b == std::byte{0xff}; });
  }

  std::string_view Record::payload() const {
    if (size() > kPayloadSize) {
      return {};
    }
    return {reinterpret_cast<const char*>(bytes.data() + 8), size()};
  }

  bool valid_serial(std::string_view value) {
    return !value.empty() && value.size() <= kPayloadSize &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= 0x20 && c <= 0x7e; });
  }

  Record serial_record(std::string_view value,
                       const util::crc32::Service& crc) {
    Record record;
    record.bytes.fill(std::byte{0xff});
    if (!valid_serial(value)) {
      return record;
    }
    record.bytes[0] = std::byte{1};
    record.bytes[1] = std::byte{0};
    record.bytes[2] = std::byte(value.size());
    record.bytes[3] = std::byte{0};
    std::copy_n(reinterpret_cast<const std::byte*>(value.data()), value.size(),
                record.bytes.data() + 8);
    auto checksum = Checksum(record, crc);
    for (std::size_t i = 0; i < 4; ++i) {
      record.bytes[4 + i] = std::byte((checksum >> (8 * i)) & 0xff);
    }
    return record;
  }


}  // namespace daveos::otp
