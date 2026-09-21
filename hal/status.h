#pragma once

#include <cstdint>

#include "core/enum/enum.h"

namespace daveos::hal {


#define DAVEOS_HAL_STATUS_VALUES(X) \
  X(ok)                             \
  X(busy)                           \
  X(invalid_argument)               \
  X(unsupported_buffer)             \
  X(not_initialized)                \
  X(initialization_failed)          \
  X(timeout)                        \
  X(timer_error)                    \
  X(nack)                           \
  X(arbitration_lost)               \
  X(hardware_error)                 \
  X(response_mismatch)              \
  X(faulted)
  DAVEOS_ENUM(Status, std::uint8_t, DAVEOS_HAL_STATUS_VALUES)
#undef DAVEOS_HAL_STATUS_VALUES


}  // namespace daveos::hal
