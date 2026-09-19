#pragma once

#include "application.h"
#include "net/module.hpp"

namespace app {
  // Service stays independent of DaveOS; the module supplies scheduled polling.
  struct Network {
    explicit Network(Platform& platform);
    void stop();
    daveos::net::Service service;
    daveos::net::Module<Event> module;
  };
}  // namespace app
