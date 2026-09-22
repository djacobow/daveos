#pragma once

#include <cstdint>
#include <limits>

namespace daveos::util {


  // Diagnostic identity only. Installation counters, never version ordering,
  // determine boot preference. Fixed storage can be copied to retained RAM.
  struct Version {
    static constexpr auto kLocal = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t build = kLocal;
    char commit[41]{};
    bool dirty = false;
  };


}  // namespace daveos::util
