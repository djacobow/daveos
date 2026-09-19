#include "counter.hpp"

#include "core/schedule/application.hpp"
#include "platform/fake/platform.h"

int main() {
  daveos::platform::fake::Platform platform;
  app::Counter counter;
  auto application =
      app::core::make_application(platform, app::core::ModuleList{&counter});
  // No wall-clock delay: automatic fake time advances through three ticks.
  const auto status = application.run();
  return status == app::core::Status::ok &&
                 counter.status() == app::core::Status::ok &&
                 counter.count() == 3 && platform.now() == 3000
             ? 0
             : 1;
}
