#include "crc32.h"

#include "stm32h563xx.h"

namespace daveos::platform::stm32h5 {


  namespace {
    std::uint32_t Update(void*, std::uint32_t value,
                         std::span<const std::byte> bytes) {
      if (bytes.empty()) {
        return value;
      }
      RCC->AHB1ENR = RCC->AHB1ENR | RCC_AHB1ENR_CRCEN;
      (void)RCC->AHB1ENR;
      CRC->POL = 0x04c11db7;
      CRC->INIT = __RBIT(~value);
      // Reflect each byte, then reflect the result; software supplies xorout.
      CRC->CR = CRC_CR_REV_IN_0 | CRC_CR_REV_OUT | CRC_CR_RESET;
      for (auto byte : bytes) {
        *reinterpret_cast<volatile std::uint8_t*>(&CRC->DR) =
            std::to_integer<std::uint8_t>(byte);
      }
      return ~CRC->DR;
    }

    bool InInterrupt(void*) { return __get_IPSR() != 0; }
  }  // namespace

  util::crc32::Backend crc32_backend() {
    return {nullptr, Update, InInterrupt};
  }


}  // namespace daveos::platform::stm32h5
