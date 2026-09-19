#include "counter.hpp"

#include "core/schedule/scheduler.hpp"
#include "platform/fake/platform.h"

int main() {
  daveos::platform::fake::Platform platform;
  app::Counter counter;
  auto scheduler = app::core::make_scheduler<app::Event>(
      platform, app::core::ModuleList{&counter});
  // No wall-clock delay: automatic fake time advances through three ticks.
  const auto status = scheduler.run();
  return status == app::core::Status::ok &&
                 counter.status() == app::core::Status::ok &&
                 counter.count() == 3 && platform.now() == 3000
             ? 0
             : 1;
}
