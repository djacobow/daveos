#pragma once

#include "display.hpp"
#include "util/version_stamp.h"

namespace app {


  // Example content is independent of optional network/OTP modules. Five
  // middle rows are available; H755 has no provisioned serial-number service.
  inline void DebugDisplayLine(void*, std::size_t row, std::span<char> output) {
    const auto& version = daveos::build::kApplicationVersion;
    switch (row) {
      case 0:
        std::snprintf(output.data(), output.size(), "DaveOS %s", board::kName);
        break;
      case 1:
        std::snprintf(output.data(), output.size(), "Git %.8s%s",
                      version.commit, version.dirty ? " dirty" : "");
        break;
      default:
        break;
    }
  }

  using DebugDisplay = daveos::drivers::DebugDisplay<Platform, Display, Event>;


}  // namespace app
