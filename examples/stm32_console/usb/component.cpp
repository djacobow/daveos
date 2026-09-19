#include "component.h"

#include "../statistics.h"

namespace app {
  Usb::Usb(Platform& platform) : transport(platform), module(transport) {}

  void Usb::stop() { transport.stop(); }

  void Usb::log_statistics(core::SchedulerInterface<Event>& scheduler) {
    LogTx(scheduler, "USB TX", transport.counters());
  }
}  // namespace app
