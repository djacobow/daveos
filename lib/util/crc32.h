#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace daveos::util::crc32 {


  // CRC-32/ISO-HDLC, with the same initial-zero/external-state convention as
  // Python binascii.crc32. Each caller owns its state; empty updates preserve
  // it. No alignment requirement, allocation, global state, or peripheral
  // access.
  constexpr std::uint32_t update(std::uint32_t value,
                                 std::span<const std::byte> bytes) {
    auto crc = ~value;
    for (auto byte : bytes) {
      crc ^= std::to_integer<std::uint8_t>(byte);
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
      }
    }
    return ~crc;
  }

  constexpr std::uint32_t calculate(std::span<const std::byte> bytes) {
    return update(0, bytes);
  }

  // Optional borrowed hardware accelerator. A backend restores the supplied
  // state on every call and serializes peripheral use internally. Interrupt
  // calls never enter the hardware backend; independent states may interleave.
  struct Backend {
    void* context = nullptr;
    std::uint32_t (*update)(void*, std::uint32_t,
                            std::span<const std::byte>) = nullptr;
    bool (*in_interrupt)(void*) = nullptr;
  };

  class Service {
   public:
    constexpr explicit Service(const Backend& backend = {})
        : backend_(backend) {}

    std::uint32_t update(std::uint32_t value,
                         std::span<const std::byte> bytes) const {
      if (backend_.update && backend_.in_interrupt &&
          !backend_.in_interrupt(backend_.context)) {
        return backend_.update(backend_.context, value, bytes);
      }
      return crc32::update(value, bytes);
    }

    std::uint32_t calculate(std::span<const std::byte> bytes) const {
      return update(0, bytes);
    }

   private:
    Backend backend_;
  };


}  // namespace daveos::util::crc32
