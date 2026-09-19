#include <cstdio>

#include "core/schedule/scheduler.hpp"
#include "counter.hpp"
#include "platform/host/platform.h"

namespace app {
  Counter counter;
}  // namespace app

int main() {
  daveos::platform::host::Platform platform;
  auto scheduler = app::core::make_scheduler<app::Event>(
      platform, app::core::ModuleList{&app::counter});
  const auto status = scheduler.run();
  if (status != app::core::Status::ok ||
      app::counter.status() != app::core::Status::ok) {
    std::fprintf(stderr, "DaveOS: %s; module: %s\n",
                 app::core::enum_name(status),
                 app::core::enum_name(app::counter.status()));
    return 1;
  }
  std::puts("Three ticks completed");
  return 0;
}
