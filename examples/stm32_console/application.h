#pragma once

#include "board_config.h"
#include "core/schedule/module.hpp"

namespace app {
  namespace core = daveos::core;
  using Platform = board::Platform;
  enum class Event {};

  // Bind sources only after every selected transport has completed stage1.
  class CommandWiring final : public core::Module<CommandWiring, Event> {
   public:
    static constexpr const char* name() { return "commands"; }

    core::Status init(core::InitStage stage);
  };
}  // namespace app
