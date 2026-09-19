#pragma once

#include <inttypes.h>

#include "core/schedule/module.hpp"

namespace app {


  namespace core = daveos::core;
  using Event = daveos::core::NoEvent;

  // Replace Poll's body with bounded application work. No hardware is touched
  // during construction, and no init override is needed just to start a task.
  class Worker : public core::Module<Worker> {
   public:
    static constexpr const char* name() { return "worker"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_PERIODIC(Worker, Poll, std::chrono::seconds{1})};
    }

   private:
    void Poll() { I_("tick %" PRIu32, ++ticks_); }

    std::uint32_t ticks_ = 0;
  };


}  // namespace app
