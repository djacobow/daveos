#pragma once

#include "otp_ecc.h"

namespace daveos::platform::stm32h5::detail {


  // Internal single-core interrupt bridge. Accesses are synchronous and are
  // not permitted from other callbacks/interrupt handlers during a read.
  struct Halfword {
    std::uint16_t value;
    Cell state;
  };

  bool flash_busy();
  Halfword read_otp(std::uint32_t address);


}  // namespace daveos::platform::stm32h5::detail
