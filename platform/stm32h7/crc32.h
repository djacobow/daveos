#pragma once

#include "util/crc32.h"

namespace daveos::platform::stm32h7 {


  // Exclusive owner of CRC peripheral configuration. All interrupt requests
  // through the returned service use software; foreground callers serialize
  // naturally on the cooperative scheduler. Construction touches no hardware.
  util::crc32::Backend crc32_backend();


}  // namespace daveos::platform::stm32h7
