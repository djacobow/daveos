#pragma once

#include <array>

#include "core/platform/platform.hpp"

namespace daveos::core {


  // Progress within the current scheduling generation, independent of
  // resettable diagnostic statistics. first_due/period describe actual cadence.
  struct TaskProgress {
    const char* module = "";
    const char* task = "";
    bool repeating = false;
    Time first_due = 0;
    Time period = 0;
    std::uint64_t completed = 0;
    std::uint64_t generation = 0;
  };

  template <std::size_t Tasks>
  struct Progress {
    bool running = false;
    Time now = 0;
    std::array<TaskProgress, Tasks> tasks{};
  };


}  // namespace daveos::core
