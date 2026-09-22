#pragma once

// The application's selected components, for interrupt routing sources. Each
// feature's IRQ file is compiled only when that feature is selected.
#include "composition.hpp"

namespace app {
  extern Components components;
}  // namespace app
