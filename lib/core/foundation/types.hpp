#pragma once

#include <cstdint>
#include <limits>

#include "core/enum/enum.h"

// Value types shared by the kernel, services and standalone drivers. Nothing
// here depends on the platform contract, the scheduler or logging.
namespace daveos::core {


  // Monotonic microseconds: timestamps, delays, and measured durations.
  using Time = std::uint64_t;
  // Sentinel for no deadline or an indefinite wait; never a real timestamp.
  inline constexpr Time kForever = std::numeric_limits<Time>::max();
// Shared operation results. Failures normally leave the requested work
// unchanged; truncated is the exception: the shortened log record has been
// accepted.
#define DAVEOS_STATUS_VALUES(X) \
  X(ok)                         \
  X(not_running)                \
  X(already_initialized)        \
  X(already_run)                \
  X(initialization_failed)      \
  X(invalid_argument)           \
  X(not_found)                  \
  X(full)                       \
  X(empty)                      \
  X(busy)                       \
  X(duplicate_name)             \
  X(truncated)                  \
  X(parse_error)                \
  X(ambiguous_match)            \
  X(line_too_long)              \
  X(too_many_arguments)         \
  X(unsupported)                \
  X(health_failed)              \
  X(io_error)                   \
  X(timeout)                    \
  X(checksum_error)             \
  X(incompatible)               \
  X(not_confirmed)              \
  X(rejected)                   \
  X(counter_exhausted)          \
  X(invalid_context)            \
  X(depth_limit)
  DAVEOS_ENUM(Status, std::int32_t, DAVEOS_STATUS_VALUES)
#undef DAVEOS_STATUS_VALUES


}  // namespace daveos::core
