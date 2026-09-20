#pragma once

#include <cstdint>

namespace daveos::platform::stm32h5::detail {


  inline constexpr std::uint32_t kOtpBase = 0x08fff000, kOtpSize = 2048;
  inline constexpr std::uint32_t kEccd = 0x80000000, kEccc = 0x40000000;
  inline constexpr std::uint32_t kOtpFlag = 0x01000000;
  inline constexpr std::uint32_t kRegionMask = 0x01f00000;
  inline constexpr std::uint32_t kBankFlag = 0x00400000;
  inline constexpr std::uint32_t kOtpEccAddressBase = 0x600;
  enum class ReadRegion { none, flash, otp };
  enum class Cell { programmed, virgin, unreadable };

  // H563 ADDR_ECC uses 128-bit main-flash words. OTP reports a 32-bit
  // address group with bias 0x600; reads still MUST use 16-bit accesses.
  // A guard never excuses an event from another region, bank or address.
  constexpr bool matches(ReadRegion region, std::uint32_t address,
                         std::uint32_t size, std::uint32_t status) {
    if (!(status & kEccd) || !size) {
      return false;
    }
    auto encoded = status & 0xffff;
    if (region == ReadRegion::otp) {
      return size == 2 && address >= kOtpBase &&
             address < kOtpBase + kOtpSize && !(address & 1) &&
             (status & kRegionMask) == kOtpFlag &&
             encoded == kOtpEccAddressBase + (address - kOtpBase) / 4;
    }
    if (region == ReadRegion::flash && !(status & (kRegionMask & ~kBankFlag))) {
      auto failed =
          0x08000000 + ((status & kBankFlag) ? 0x100000 : 0) + encoded * 16;
      return address >= 0x08000000 && address < 0x08200000 &&
             size <= 0x08200000 - address && failed < address + size &&
             failed + 16 > address;
    }
    return false;
  }

  constexpr Cell classify(std::uint16_t value, bool eccd,
                          std::uint16_t failed_data, bool corrected) {
    if (corrected) {
      return Cell::unreadable;
    }
    if (!eccd) {
      return Cell::programmed;  // Includes a successfully read programmed
                                // 0xffff.
    }
    return value == 0xffff && failed_data == 0xffff ? Cell::virgin
                                                    : Cell::unreadable;
  }


}  // namespace daveos::platform::stm32h5::detail
