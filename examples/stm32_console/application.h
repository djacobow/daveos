#pragma once

#include "board_config.h"
#include "core/schedule/module.hpp"

namespace app {
  namespace core = daveos::core;
  using Platform = board::Platform;
  using Event = std::variant<std::monostate>;

}  // namespace app
