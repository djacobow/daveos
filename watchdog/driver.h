#pragma once

#include "core/platform/platform.hpp"

namespace daveos::watchdog {


  // Borrowed hardware driver, independent of scheduler/module templates.
  struct Driver {
    void* context = nullptr;
    core::Status (*start)(void*, core::Time) = nullptr;
    core::Status (*feed)(void*) = nullptr;
  };


}  // namespace daveos::watchdog
