#pragma once

#include "../application.h"
#include "usb.hpp"

namespace app {
  // Keep transport lifetime longer than its borrowing console module.
  struct Usb {
    explicit Usb(Platform& platform);
    void stop();
    void log_statistics(core::SchedulerInterface<Event>& scheduler);
    board::UsbTransport transport;
    board::UsbConsole<Event> module;
  };
}  // namespace app
